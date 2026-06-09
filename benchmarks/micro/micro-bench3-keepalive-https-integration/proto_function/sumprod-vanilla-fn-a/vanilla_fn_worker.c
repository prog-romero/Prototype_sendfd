/*
 * vanilla_fn_worker.c — Keep-Alive HTTP Vanilla worker using epoll
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define CNTFRQ 1000000000ULL

/* ── Monotonic nanoseconds ───────────────────────────────────────────────── */

static uint64_t get_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── HTTP parsing helpers ────────────────────────────────────────────────── */

static ssize_t find_seq(const char *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static const char *extract_header(const char *headers, size_t hlen,
                                  const char *name, size_t *vlen)
{
    size_t nlen = strlen(name);
    const char *p = headers, *end = headers + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll > nlen + 1) {
            int match = 1;
            for (size_t k = 0; k < nlen && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                char n = name[k];
                if (n >= 'A' && n <= 'Z') n = (char)(n - 'A' + 'a');
                match = (c == n);
            }
            if (match && p[nlen] == ':') {
                const char *v = p + nlen + 1;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                const char *ve = v;
                while (ve < end && *ve != '\r' && *ve != '\n') ve++;
                *vlen = (size_t)(ve - v);
                return v;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return NULL;
}

static long long parse_content_length(const char *headers, size_t hlen)
{
    size_t vlen = 0;
    const char *v = extract_header(headers, hlen, "Content-Length", &vlen);
    if (!v) return -1;
    char tmp[32];
    size_t copy = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
    memcpy(tmp, v, copy);
    tmp[copy] = '\0';
    char *ep = NULL;
    long long val = strtoll(tmp, &ep, 10);
    return (ep && ep > tmp) ? val : -1;
}

static int should_close(const char *headers, size_t hlen)
{
    size_t vlen = 0;
    const char *v = extract_header(headers, hlen, "Connection", &vlen);
    if (!v) return 0;
    char tmp[16];
    size_t copy = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
    memcpy(tmp, v, copy);
    tmp[copy] = '\0';
    for (size_t i = 0; tmp[i]; i++)
        if (tmp[i] >= 'A' && tmp[i] <= 'Z') tmp[i] = (char)(tmp[i] - 'A' + 'a');
    return strstr(tmp, "close") != NULL;
}

/* ── Epoll Multi-Connection State Machine ────────────────────────────────── */

#define MAX_EVENTS 128
#define MAX_FDS 65536

typedef enum {
    WS_READ_HEADERS = 0,
    WS_READ_BODY,
    WS_WRITE_RESPONSE,
} worker_state_t;

typedef struct {
    int fd;
    worker_state_t state;
    uint64_t req_no;
    unsigned char *buf;
    size_t cap;
    size_t len;
    size_t hdr_sz;
    size_t body_in;
    size_t body_target;
    int should_close;
    char resp[768];
    size_t resp_len;
    size_t resp_off;
} worker_session_t;

static int s_epoll_fd = -1;
static const char *s_function_name = NULL;
static int s_is_product = 0;

static worker_session_t *s_sessions[MAX_FDS];

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static int epoll_add_fd(int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod_fd(int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static int ensure_capacity(unsigned char **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) return 0;
    size_t new_cap = *cap ? *cap : 16384;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    unsigned char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return -1;
    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

static void session_close(worker_session_t *s)
{
    if (!s) return;
    int fd = s->fd;
    if (fd >= 0 && fd < MAX_FDS) {
        s_sessions[fd] = NULL;
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
    if (fd >= 0) close(fd);
    free(s->buf);
    free(s);
}

static int session_build_response(worker_session_t *s)
{
    int num1 = 0, num2 = 0;
    int result = 0;
    int parse_ok = 0;
    if (sscanf((const char *)(s->buf + s->hdr_sz), "%d %d", &num1, &num2) >= 2) {
        if (s_is_product) {
            result = num1 * num2;
        } else {
            result = num1 + num2;
        }
        parse_ok = 1;
    }

    char json[512];
    int jl;
    if (parse_ok) {
        jl = snprintf(json, sizeof(json),
            "{\n"
            "  \"status\": \"success\",\n"
            "  \"result\": %d\n"
            "}\n",
            result);
    } else {
        jl = snprintf(json, sizeof(json),
            "{\n"
            "  \"status\": \"error\",\n"
            "  \"message\": \"Parsing failed.\",\n"
            "  \"raw_body\": \"%s\"\n"
            "}\n",
            (const char *)(s->buf + s->hdr_sz));
    }

    int rl = snprintf(s->resp, sizeof(s->resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n%s",
        jl,
        s->should_close ? "close" : "keep-alive",
        json);

    s->resp_len = (size_t)rl;
    s->resp_off = 0;
    s->state = WS_WRITE_RESPONSE;

    return epoll_mod_fd(s->fd, EPOLLOUT);
}

static int session_advance(worker_session_t *s)
{
    for (;;) {
        switch (s->state) {
        case WS_READ_HEADERS: {
            if (ensure_capacity(&s->buf, &s->cap, s->len + 4096 + 1) != 0) return -1;
            ssize_t n = recv(s->fd, s->buf + s->len, s->cap - s->len - 1, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            if (n == 0) return -1;
            s->len += (size_t)n;
            s->buf[s->len] = '\0';

            ssize_t hdr_end = find_seq((const char *)s->buf, s->len, "\r\n\r\n");
            size_t sep_len = 4;
            if (hdr_end < 0) {
                hdr_end = find_seq((const char *)s->buf, s->len, "\n\n");
                sep_len = 2;
            }
            if (hdr_end < 0) return 0;

            s->hdr_sz = (size_t)hdr_end + sep_len;
            long long cl = parse_content_length((const char *)s->buf, s->hdr_sz);
            if (cl < 0) cl = 0;
            s->body_target = (size_t)cl;

            s->body_in = (s->len > s->hdr_sz) ? s->len - s->hdr_sz : 0;
            if (s->body_in > s->body_target) s->body_in = s->body_target;

            s->should_close = should_close((const char *)s->buf, s->hdr_sz);
            s->state = WS_READ_BODY;
            continue;
        }
        case WS_READ_BODY: {
            while (s->body_in < s->body_target) {
                if (ensure_capacity(&s->buf, &s->cap, s->len + 4096 + 1) != 0) return -1;
                size_t want = s->body_target - s->body_in;
                if (want > 4096) want = 4096;

                ssize_t n = recv(s->fd, s->buf + s->len, want, MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                    return -1;
                }
                if (n == 0) return -1;

                s->len += (size_t)n;
                s->body_in += (size_t)n;
                s->buf[s->len] = '\0';
            }
            return session_build_response(s);
        }
        case WS_WRITE_RESPONSE: {
            while (s->resp_off < s->resp_len) {
                ssize_t n = send(s->fd, s->resp + s->resp_off, s->resp_len - s->resp_off, MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                    return -1;
                }
                if (n == 0) return -1;
                s->resp_off += (size_t)n;
            }

            if (s->should_close) return -1;

            s->req_no++;
            s->len = 0;
            s->hdr_sz = 0;
            s->body_in = 0;
            s->body_target = 0;
            s->state = WS_READ_HEADERS;

            if (epoll_mod_fd(s->fd, EPOLLIN) != 0) return -1;
            return 0;
        }
        }
    }
}

static void handle_listen_fd(int listen_fd)
{
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (conn_fd >= MAX_FDS) {
            close(conn_fd);
            continue;
        }

        worker_session_t *s = calloc(1, sizeof(*s));
        if (!s) {
            close(conn_fd);
            continue;
        }

        s->fd = conn_fd;
        s->cap = 16384;
        s->buf = malloc(s->cap);
        if (!s->buf) {
            free(s);
            close(conn_fd);
            continue;
        }

        s->req_no = 1;
        s->state = WS_READ_HEADERS;
        s_sessions[conn_fd] = s;

        if (epoll_add_fd(conn_fd, EPOLLIN) != 0) {
            session_close(s);
        }
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name  = getenv("VANILLA_FN_NAME");
    const char *port_str = getenv("VANILLA_FN_PORT");
    if (!fn_name  || !fn_name[0])  fn_name  = "vanilla-fn-a";
    if (!port_str || !port_str[0]) port_str = "8080";

    int port = atoi(port_str);
    if (port <= 0) port = 8080;

    s_is_product = (strstr(fn_name, "-b") != NULL || strstr(fn_name, "prod") != NULL);
    s_function_name = fn_name;

    fprintf(stderr, "[vanilla-fn] %s starting on port %d (%s)\n", fn_name, port, s_is_product ? "product" : "sum");

    int listenfd = socket(AF_INET6, SOCK_STREAM, 0);
    int use_ipv4 = 0;
    if (listenfd < 0) {
        listenfd = socket(AF_INET, SOCK_STREAM, 0);
        use_ipv4 = 1;
        if (listenfd < 0) { perror("socket"); return 1; }
    }

    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!use_ipv4)
        setsockopt(listenfd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){0}, sizeof(int));

    if (use_ipv4) {
        struct sockaddr_in sa4;
        memset(&sa4, 0, sizeof(sa4));
        sa4.sin_family = AF_INET;
        sa4.sin_port   = htons((uint16_t)port);
        sa4.sin_addr.s_addr = INADDR_ANY;
        if (bind(listenfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0) {
            perror("bind"); close(listenfd); return 1;
        }
    } else {
        struct sockaddr_in6 sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port   = htons((uint16_t)port);
        if (bind(listenfd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0) {
            perror("bind"); close(listenfd); return 1;
        }
    }

    if (listen(listenfd, 1024) < 0) {
        perror("listen"); close(listenfd); return 1;
    }

    if (set_nonblocking(listenfd) != 0) {
        close(listenfd);
        return 1;
    }

    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) {
        close(listenfd);
        return 1;
    }

    if (epoll_add_fd(listenfd, EPOLLIN) != 0) {
        close(s_epoll_fd);
        close(listenfd);
        return 1;
    }

    fprintf(stderr, "[vanilla-fn] %s listening on :%d [epoll multi-session]\n", fn_name, port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(s_epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t rev = events[i].events;

            if (fd == listenfd) {
                handle_listen_fd(listenfd);
                continue;
            }

            if (fd < 0 || fd >= MAX_FDS) continue;

            worker_session_t *s = s_sessions[fd];
            if (!s) {
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }

            if (rev & (EPOLLIN | EPOLLOUT)) {
                if (session_advance(s) < 0) {
                    session_close(s);
                }
            } else {
                session_close(s);
            }
        }
    }

    close(s_epoll_fd);
    close(listenfd);
    return 0;
}
