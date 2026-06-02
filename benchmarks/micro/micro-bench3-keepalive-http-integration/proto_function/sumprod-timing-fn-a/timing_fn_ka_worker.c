/*
 * timing_fn_ka_worker.c — Keep-Alive HTTP integration worker using epoll
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "sendfd.h"
#include "unix_socket.h"

/* ── Timing: nanoseconds via CLOCK_MONOTONIC_RAW ─────────────────────────── */

static uint64_t get_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            perror("[fn-worker] clock_gettime");
            return 0;
        }
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#define CNTFRQ 1000000000ULL

/* ── Payload (must match gateway's KAPayload struct) ─────────────────────── */

#define HTTPMIGRATE_MAGIC   0x484D4B41U   /* 'HMKA' */
#define HTTPMIGRATE_VERSION 2U
#define HTTPMIGRATE_TARGET_LEN 128

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;   /* nanoseconds in integration bench */
    uint64_t cntfrq;       /* always 1_000_000_000             */
    uint8_t  top1_set;
    uint8_t  _pad[7];
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} httpmigrate_ka_payload_t;

/* ── Non-blocking 2-FD receive ──────────────────────────────────────────── */

static int recvfd2_with_state_nb(int unix_sock,
                                 int *fd1_out, int *fd2_out,
                                 void *payload, size_t payload_len)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 2)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct iovec iov = {
        .iov_base = payload,
        .iov_len  = payload_len,
    };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t n;
    do {
        n = recvmsg(unix_sock, &msg, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; // would block
        return -1; // error
    }
    if (n == 0) return -1; // peer closed

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }

    size_t nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    int *fds = (int *)CMSG_DATA(cmsg);

    if (nfds < 2) {
        for (size_t i = 0; i < nfds; i++) close(fds[i]);
        return -1;
    }

    *fd1_out = fds[0]; /* clientFD   */
    *fd2_out = fds[1]; /* pipeWriteFD */

    for (size_t i = 2; i < nfds; i++) close(fds[i]);

    return 1; // success
}

/* ── Container IP discovery ──────────────────────────────────────────────── */

static int get_container_ip(char *buf, size_t bufsz)
{
    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0) return -1;

    int found = 0;
    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, (socklen_t)bufsz)) {
            found = 1;
            break;
        }
    }
    freeifaddrs(ifa_list);
    return found ? 0 : -1;
}

/* ── HTTP parsing helpers ────────────────────────────────────────────────── */

static ssize_t find_subseq(const unsigned char *buf, size_t len,
                            const char *needle)
{
    const size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static long long parse_content_length(const char *headers, size_t hlen)
{
    const char *p = headers, *end = headers + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *needle = "content-length:";
        size_t nlen = strlen(needle);
        if (ll >= nlen) {
            int m = 1;
            for (size_t k = 0; k < nlen && m; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == needle[k]);
            }
            if (m) {
                const char *v = p + nlen;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                char *ep = NULL;
                long long val = strtoll(v, &ep, 10);
                if (ep && ep > v) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return -1;
}

static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *out, size_t outsz)
{
    if (!buf || !len || !out || !outsz) return false;
    size_t eol = 0;
    while (eol < len && buf[eol] != '\n') eol++;
    const unsigned char *sp1 = memchr(buf, ' ', eol);
    if (!sp1) return false;
    const unsigned char *path = sp1 + 1;
    static const char prefix[] = "/function/";
    size_t plen = eol - (size_t)(path - buf);
    const unsigned char *sp2 = memchr(path, ' ', plen);
    if (!sp2) return false;
    size_t rlen = (size_t)(sp2 - path);
    if (rlen <= sizeof(prefix) - 1 ||
        memcmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++)
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            nlen = i;
            break;
        }
    if (!nlen || nlen + 1 > outsz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

/* ── Epoll Multi-Connection State Machine ────────────────────────────────── */

#define MAX_EVENTS 128
#define MAX_FDS 65536

typedef enum {
    WS_PEEK_OWNER = 0,
    WS_READ_HEADERS,
    WS_READ_BODY,
    WS_WRITE_RESPONSE,
} worker_state_t;

typedef struct {
    int fd;
    int pipe_fd;
    bool pipe_sent;
    httpmigrate_ka_payload_t payload;
    worker_state_t state;
    uint64_t req_no;
    unsigned char *buf;
    size_t cap;
    size_t len;
    size_t hdr_sz;
    size_t body_in;
    size_t body_target;
    int should_close;
    char resp[4096];
    size_t resp_len;
    size_t resp_off;
    bool first_request;
} worker_session_t;

typedef struct {
    int fd;
    int client_fd;
    int pipe_wr_fd;
    httpmigrate_ka_payload_t payload;
} handoff_conn_t;

static int s_epoll_fd = -1;
static const char *s_function_name = NULL;
static const char *s_relay_socket = NULL;
static int s_is_product = 0;

static worker_session_t *s_sessions[MAX_FDS];
static handoff_conn_t *s_handoffs[MAX_FDS];

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
    if (!s->pipe_sent && s->pipe_fd >= 0) {
        uint64_t ts_le = get_ns();
        (void)write(s->pipe_fd, &ts_le, sizeof(ts_le));
        close(s->pipe_fd);
    }
    free(s->buf);
    free(s);
}

static void handoff_close(int fd)
{
    if (fd >= 0 && fd < MAX_FDS) {
        handoff_conn_t *h = s_handoffs[fd];
        s_handoffs[fd] = NULL;
        if (h) {
            if (h->client_fd >= 0) close(h->client_fd);
            if (h->pipe_wr_fd >= 0) close(h->pipe_wr_fd);
            free(h);
        }
    }
    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

static int session_relay(worker_session_t *s, const char *owner)
{
    httpmigrate_ka_payload_t relay_payload;
    memset(&relay_payload, 0, sizeof(relay_payload));
    relay_payload.magic      = HTTPMIGRATE_MAGIC;
    relay_payload.version    = HTTPMIGRATE_VERSION;
    relay_payload.top1_rdtsc = get_ns();
    relay_payload.cntfrq     = CNTFRQ;
    relay_payload.top1_set   = 1;
    snprintf(relay_payload.target_function, sizeof(relay_payload.target_function), "%s", owner);

    int fd_to_relay = s->fd;
    s->fd = -1; // prevent session_close from closing it

    if (fd_to_relay >= 0 && fd_to_relay < MAX_FDS) {
        s_sessions[fd_to_relay] = NULL;
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd_to_relay, NULL);
    }

    int relay_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (relay_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s_relay_socket, sizeof(addr.sun_path) - 1);
        if (connect(relay_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(relay_fd);
            relay_fd = -1;
        }
    }

    if (relay_fd >= 0) {
        sendfd_with_state(relay_fd, fd_to_relay, &relay_payload, sizeof(relay_payload));
        close(relay_fd);
    } else {
        close(fd_to_relay);
    }

    session_close(s);
    return 0;
}

static int session_peek_owner(worker_session_t *s)
{
    unsigned char peek_buf[1024];
    ssize_t pn = recv(s->fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK | MSG_DONTWAIT);
    if (pn < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (pn == 0) return -1;

    peek_buf[pn] = '\0';

    if (memchr(peek_buf, '\n', pn) == NULL) {
        return 0; // Partial line, wait for more data
    }

    char owner[HTTPMIGRATE_TARGET_LEN];
    if (!parse_request_owner(peek_buf, (size_t)pn, owner, sizeof(owner))) {
        fprintf(stderr, "[fn-worker] parse_request_owner FAILED on %d bytes: %.*s\n", (int)pn, (int)pn, peek_buf);
        return -1;
    }

    if (strcmp(owner, s_function_name) != 0) {
        fprintf(stderr, "[fn-worker] Relay triggered! Parsed owner='%s', but s_function_name='%s'\n", owner, s_function_name);
        (void)session_relay(s, owner);
        return 2;
    }

    s->state = WS_READ_HEADERS;
    s->len = 0;
    return 1;
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

    char json[2048];
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
            "  \"raw_body\": \"%.512s\"\n" // truncate body in output just in case
            "}\n",
            (const char *)(s->buf + s->hdr_sz));
    }
    if (jl < 0) jl = 0;
    if ((size_t)jl >= sizeof(json)) jl = sizeof(json) - 1;

    int rl = snprintf(s->resp, sizeof(s->resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n%s",
        jl,
        s->should_close ? "close" : "keep-alive",
        json);
        
    if (rl < 0) rl = 0;
    if ((size_t)rl >= sizeof(s->resp)) rl = sizeof(s->resp) - 1;

    s->resp_len = (size_t)rl;
    s->resp_off = 0;
    s->state = WS_WRITE_RESPONSE;

    if (!s->pipe_sent && s->pipe_fd >= 0) {
        uint64_t ts_le = 0; // Dummy zero instead of top2
        (void)write(s->pipe_fd, &ts_le, sizeof(ts_le));
        close(s->pipe_fd);
        s->pipe_fd = -1;
        s->pipe_sent = true;
    }

    if (epoll_mod_fd(s->fd, EPOLLOUT | EPOLLET) != 0) return -1;
    return 1; // Continue state machine immediately to avoid EPOLLET hangs!
}

static int session_advance(worker_session_t *s)
{
    for (;;) {
        switch (s->state) {
        case WS_PEEK_OWNER: {
            int rc = session_peek_owner(s);
            if (rc == 2) return 0;
            if (rc <= 0) return rc;
            continue;
        }
        case WS_READ_HEADERS: {
            if (ensure_capacity(&s->buf, &s->cap, 4096 + 1) != 0) return -1;
            
            // Peek the socket to check for full headers
            ssize_t n = recv(s->fd, s->buf, s->cap - 1, MSG_PEEK | MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            if (n == 0) return -1;
            s->buf[n] = '\0';

            ssize_t hdr_end = find_subseq(s->buf, (size_t)n, "\r\n\r\n");
            size_t sep_len = 4;
            if (hdr_end < 0) {
                hdr_end = find_subseq(s->buf, (size_t)n, "\n\n");
                sep_len = 2;
            }
            if (hdr_end < 0) {
                if ((size_t)n >= s->cap - 1) return -1; // Header buffer overflow
                return 0; // Wait for more header bytes
            }

            s->hdr_sz = (size_t)hdr_end + sep_len;

            // Drain EXACTLY the headers from the socket
            ssize_t read_bytes = recv(s->fd, s->buf, s->hdr_sz, MSG_DONTWAIT);
            if (read_bytes < 0) return -1;
            s->len = (size_t)read_bytes;
            s->buf[s->len] = '\0';

            long long cl = parse_content_length((const char *)s->buf, s->hdr_sz);
            if (cl < 0) cl = 0;
            s->body_target = (size_t)cl;
            s->body_in = 0;

            s->should_close = 0;
            if (find_subseq(s->buf, s->hdr_sz, "Connection: close") >= 0 ||
                find_subseq(s->buf, s->hdr_sz, "connection: close") >= 0) {
                s->should_close = 1;
            }

            if (s->body_target > 0) {
                s->state = WS_READ_BODY;
                continue;
            }
            return session_build_response(s);
        }
        case WS_READ_BODY: {
            size_t want = s->body_target - s->body_in;
            if (want > 0) {
                if (ensure_capacity(&s->buf, &s->cap, s->len + want + 1) != 0) return -1;
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
            if (s->body_in < s->body_target) continue;
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
            s->first_request = false;
            s->len = 0;
            s->hdr_sz = 0;
            s->body_in = 0;
            s->body_target = 0;
            s->state = WS_PEEK_OWNER;

            if (epoll_mod_fd(s->fd, EPOLLIN | EPOLLET) != 0) return -1;
            continue; // Immediately check for pipelined requests
        }
        }
    }
}

static void register_client_fd(int client_fd, int pipe_fd, const httpmigrate_ka_payload_t *payload)
{
    if (client_fd < 0 || client_fd >= MAX_FDS) {
        if (client_fd >= 0) close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }
    if (set_nonblocking(client_fd) != 0) {
        close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }

    worker_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }

    s->fd = client_fd;
    s->pipe_fd = pipe_fd;
    s->pipe_sent = false;
    memcpy(&s->payload, payload, sizeof(s->payload));
    s->cap = 16384;
    s->buf = malloc(s->cap);
    if (!s->buf) {
        free(s);
        close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }
    s->req_no = 0;
    s->first_request = true;
    s->should_close = 0;
    s->len = 0;
    s->hdr_sz = 0;
    s->body_in = 0;
    s->body_target = 0;
    s->resp_len = 0;
    s->resp_off = 0;
    s->state = WS_PEEK_OWNER;

    s_sessions[client_fd] = s;

    if (epoll_add_fd(client_fd, EPOLLIN | EPOLLET) != 0) {
        s_sessions[client_fd] = NULL;
        free(s->buf);
        free(s);
        close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }

    // CRITICAL FIX: EPOLLET only triggers on NEW data arriving. Since the first
    // request might already be in the socket buffer when we received the FD from 
    // the Gateway, we MUST manually start the state machine once!
    if (session_advance(s) < 0) {
        session_close(s);
    }
}

static void handle_handoff_fd(int conn_fd)
{
    handoff_conn_t *h = (conn_fd >= 0 && conn_fd < MAX_FDS) ? s_handoffs[conn_fd] : NULL;
    if (!h) {
        handoff_close(conn_fd);
        return;
    }

    int rc = recvfd2_with_state_nb(h->fd, &h->client_fd, &h->pipe_wr_fd, &h->payload, sizeof(h->payload));
    if (rc == 0) return; // would block

    int client_fd = h->client_fd;
    int pipe_wr_fd = h->pipe_wr_fd;
    httpmigrate_ka_payload_t payload = h->payload;

    if (rc > 0) {
        h->client_fd = -1;
        h->pipe_wr_fd = -1;
    }
    handoff_close(conn_fd);

    if (rc < 0) return;

    if (payload.magic != HTTPMIGRATE_MAGIC || payload.version != HTTPMIGRATE_VERSION) {
        if (client_fd >= 0) close(client_fd);
        if (pipe_wr_fd >= 0) close(pipe_wr_fd);
        return;
    }

    register_client_fd(client_fd, pipe_wr_fd, &payload);
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
        handoff_conn_t *h = calloc(1, sizeof(*h));
        if (!h) {
            close(conn_fd);
            continue;
        }
        h->fd = conn_fd;
        h->client_fd = -1;
        h->pipe_wr_fd = -1;
        s_handoffs[conn_fd] = h;
        if (epoll_add_fd(conn_fd, EPOLLIN) != 0) {
            handoff_close(conn_fd);
        }
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name    = getenv("HTTPMIGRATE_KA_FUNCTION_NAME");
    const char *socket_dir = getenv("SENDFD_SOCKET_DIR");

    if (!fn_name    || !fn_name[0])    fn_name    = "timing-fn-a";
    if (!socket_dir || !socket_dir[0]) socket_dir = "/run/tlsmigrate";

    s_is_product = (strstr(fn_name, "-b") != NULL || strstr(fn_name, "prod") != NULL);

    char own_ip[INET_ADDRSTRLEN];
    if (get_container_ip(own_ip, sizeof(own_ip)) != 0) {
        fprintf(stderr, "[fn-worker] failed to discover container IP\n");
        return 1;
    }
    fprintf(stderr, "[fn-worker] own IP = %s\n", own_ip);

    char fn_sock_path[256];
    snprintf(fn_sock_path, sizeof(fn_sock_path), "%s/%s-fn.sock", socket_dir, own_ip);

    char relay_sock_path[256];
    snprintf(relay_sock_path, sizeof(relay_sock_path), "%s/%s-relay.sock", socket_dir, own_ip);

    s_function_name = fn_name;
    s_relay_socket = relay_sock_path;

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "[fn-worker] warning: mkdir %s: %s\n", socket_dir, strerror(errno));

    unlink(fn_sock_path);
    int listen_fd = unix_server_socket(fn_sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[fn-worker] unix_server_socket(%s) failed: %s\n", fn_sock_path, strerror(errno));
        return 1;
    }
    chmod(fn_sock_path, 0777);

    char name_sock_path[256];
    snprintf(name_sock_path, sizeof(name_sock_path), "%s/%s.sock", socket_dir, fn_name);
    unlink(name_sock_path);
    if (symlink(fn_sock_path, name_sock_path) != 0) {
        fprintf(stderr, "[fn-worker] symlink %s -> %s failed: %s\n", name_sock_path, fn_sock_path, strerror(errno));
    }

    if (set_nonblocking(listen_fd) != 0) {
        close(listen_fd);
        return 1;
    }

    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) {
        close(listen_fd);
        return 1;
    }

    if (epoll_add_fd(listen_fd, EPOLLIN) != 0) {
        close(s_epoll_fd);
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "[fn-worker] %s (%s) listening on %s, relay=%s [epoll multi-session]\n",
            fn_name, s_is_product ? "product" : "sum", fn_sock_path, relay_sock_path);

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

            if (fd == listen_fd) {
                handle_listen_fd(listen_fd);
                continue;
            }

            if (fd < 0 || fd >= MAX_FDS) continue;

            if (s_handoffs[fd]) {
                if (rev & EPOLLIN) {
                    handle_handoff_fd(fd);
                } else {
                    handoff_close(fd);
                }
                continue;
            }

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
    close(listen_fd);
    return 0;
}
