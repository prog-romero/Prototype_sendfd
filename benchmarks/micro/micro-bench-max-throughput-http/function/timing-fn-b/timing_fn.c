/*
 * timing_fn.c — Simple HTTP function worker (vanilla mode, epoll multi-session)
 * Function B: computes the PRODUCT of two numbers from the request body.
 * Body format: "<a> <b>" (two space-separated doubles).
 * If parsing fails, defaults to a=1, b=2.
 *
 * Epoll-based single-process server: handles all TCP connections concurrently
 * without fork() — same architecture as the proto epoll worker.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── HTTP parsing helpers ─────────────────────────────────────────────── */

static ssize_t find_subseq(const char *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static long long parse_content_length(const char *hdr, size_t hlen)
{
    const char *p = hdr, *end = hdr + hlen;
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

static int has_connection_close(const char *hdr, size_t hlen)
{
    return find_subseq(hdr, hlen, "Connection: close") >= 0 ||
           find_subseq(hdr, hlen, "connection: close") >= 0;
}

static int has_transfer_chunked(const char *hdr, size_t hlen)
{
    return find_subseq(hdr, hlen, "Transfer-Encoding: chunked") >= 0 ||
           find_subseq(hdr, hlen, "transfer-encoding: chunked") >= 0;
}

static int chunked_complete(const char *buf, size_t len)
{
    if (find_subseq(buf, len, "\r\n0\r\n\r\n") >= 0) return 1;
    if (find_subseq(buf, len, "\n0\n\n") >= 0) return 1;
    return 0;
}

static int decode_chunked_body(const char *in, size_t in_len, char **out, size_t *out_len)
{
    size_t pos = 0, cap = in_len ? in_len : 1, used = 0;
    char *dst = malloc(cap + 1);
    if (!dst) return -1;

    while (pos < in_len) {
        size_t line_end = pos;
        while (line_end < in_len && in[line_end] != '\n') line_end++;
        if (line_end >= in_len) { free(dst); return -1; }

        size_t line_len = line_end - pos;
        if (line_len > 0 && in[pos + line_len - 1] == '\r') line_len--;

        char szbuf[64];
        if (line_len == 0 || line_len >= sizeof(szbuf)) { free(dst); return -1; }
        memcpy(szbuf, in + pos, line_len);
        szbuf[line_len] = '\0';

        char *semi = strchr(szbuf, ';');
        if (semi) *semi = '\0';

        char *ep = NULL;
        unsigned long long chunk_sz = strtoull(szbuf, &ep, 16);
        if (!ep || ep == szbuf) { free(dst); return -1; }

        pos = line_end + 1;
        if (chunk_sz == 0) break;
        if (pos + (size_t)chunk_sz > in_len) { free(dst); return -1; }

        if (used + (size_t)chunk_sz > cap) {
            size_t nc = cap * 2;
            while (nc < used + (size_t)chunk_sz) nc *= 2;
            char *nb = realloc(dst, nc + 1);
            if (!nb) { free(dst); return -1; }
            dst = nb; cap = nc;
        }
        memcpy(dst + used, in + pos, (size_t)chunk_sz);
        used += (size_t)chunk_sz;
        pos += (size_t)chunk_sz;

        if (pos < in_len && in[pos] == '\r') pos++;
        if (pos < in_len && in[pos] == '\n') pos++;
    }

    dst[used] = '\0';
    *out = dst;
    *out_len = used;
    return 0;
}

/* ── Session state machine ────────────────────────────────────────────── */

typedef enum {
    S_READ_HEADERS,
    S_READ_BODY,
    S_WRITE_RESP,
} sess_state_t;

typedef struct session {
    int          fd;
    uint64_t     req_no;
    sess_state_t state;
    /* read accumulation buffer */
    char  *buf;
    size_t cap;
    size_t len;
    /* header-parsing results */
    ssize_t   hdr_end;
    size_t    hdr_sep;
    size_t    hdr_bytes;
    long long content_length;
    int       chunked;
    size_t    body_in;
    int       should_close;
    double    a, b;
    /* response write buffer */
    char   resp[512];
    size_t resp_len;
    size_t resp_off;
} session_t;

/* ── Global epoll + session table ────────────────────────────────────── */

#define MAX_FD            65536
#define WORKER_MAX_EVENTS 64

static int        s_epoll_fd = -1;
static session_t *s_sessions[MAX_FD];

/* ── Low-level helpers ───────────────────────────────────────────────── */

static int epoll_add(int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod(int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

/* ── Session lifecycle ───────────────────────────────────────────────── */

static void session_close(session_t *s)
{
    if (!s) return;
    if (s->fd >= 0 && s->fd < MAX_FD) s_sessions[s->fd] = NULL;
    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
    close(s->fd);
    free(s->buf);
    free(s);
}

static session_t *session_new(int fd)
{
    if (fd < 0 || fd >= MAX_FD) { close(fd); return NULL; }
    session_t *s = calloc(1, sizeof(*s));
    if (!s) { close(fd); return NULL; }
    s->fd      = fd;
    s->req_no  = 1;
    s->state   = S_READ_HEADERS;
    s->hdr_end = -1;
    s->a       = 1.0;
    s->b       = 2.0;
    s_sessions[fd] = s;
    return s;
}

static void session_reset_for_next(session_t *s)
{
    s->len            = 0;
    s->hdr_end        = -1;
    s->hdr_sep        = 0;
    s->hdr_bytes      = 0;
    s->content_length = 0;
    s->chunked        = 0;
    s->body_in        = 0;
    s->should_close   = 0;
    s->a              = 1.0;
    s->b              = 2.0;
    s->resp_len       = 0;
    s->resp_off       = 0;
    s->req_no++;
    s->state          = S_READ_HEADERS;
}

/* ── Per-session non-blocking state machine ──────────────────────────── */

static void advance_session(session_t *s, const char *worker_name)
{
    int again;
    do {
        again = 0;
        switch (s->state) {

        case S_READ_HEADERS: {
            int found = 0;
            for (;;) {
                if (s->cap < s->len + 4096 + 1) {
                    size_t nc = s->cap ? s->cap * 2 : 16384;
                    char *nb = realloc(s->buf, nc);
                    if (!nb) { session_close(s); return; }
                    s->buf = nb; s->cap = nc;
                }
                ssize_t n = recv(s->fd, s->buf + s->len,
                                 s->cap - s->len - 1, MSG_DONTWAIT);
                if (n > 0) {
                    s->len += (size_t)n;
                    s->buf[s->len] = '\0';
                    ssize_t e4 = find_subseq(s->buf, s->len, "\r\n\r\n");
                    if (e4 >= 0) { s->hdr_end = e4; s->hdr_sep = 4; found = 1; break; }
                    ssize_t e2 = find_subseq(s->buf, s->len, "\n\n");
                    if (e2 >= 0) { s->hdr_end = e2; s->hdr_sep = 2; found = 1; break; }
                } else if (n == 0) {
                    session_close(s); return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    session_close(s); return;
                }
            }
            if (!found) break;
            size_t hdr_sz = (size_t)s->hdr_end + s->hdr_sep;
            s->hdr_bytes = hdr_sz;
            s->content_length = parse_content_length(s->buf, hdr_sz);
            if (s->content_length < 0) s->content_length = 0;
            s->chunked = has_transfer_chunked(s->buf, hdr_sz);
            s->should_close = has_connection_close(s->buf, hdr_sz);
            s->body_in = (s->len > hdr_sz) ? (s->len - hdr_sz) : 0;
            s->state = S_READ_BODY;
            again    = 1;
            break;
        }

        case S_READ_BODY: {
            while ((!s->chunked && s->body_in < (size_t)s->content_length) ||
                   (s->chunked && !chunked_complete(s->buf + s->hdr_bytes, s->body_in))) {
                size_t want = s->chunked ? 4096 : ((size_t)s->content_length - s->body_in);
                if (s->cap < s->len + want + 1) {
                    size_t nc = s->cap ? s->cap * 2 : 16384;
                    while (nc < s->len + want + 1) nc *= 2;
                    char *nb = realloc(s->buf, nc);
                    if (!nb) { session_close(s); return; }
                    s->buf = nb; s->cap = nc;
                }
                ssize_t n = recv(s->fd, s->buf + s->len, want, MSG_DONTWAIT);
                if (n > 0) {
                    s->len += (size_t)n;
                    s->buf[s->len] = '\0';
                    s->body_in += (size_t)n;
                } else if (n == 0) {
                    session_close(s); return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                    session_close(s); return;
                }
            }
            if (s->content_length > 0 || s->chunked) {
                const char *body = s->buf + s->hdr_bytes;
                size_t body_len = s->chunked ? s->body_in : (size_t)s->content_length;
                char *decoded = NULL;
                if (s->chunked) {
                    if (decode_chunked_body(body, body_len, &decoded, &body_len) == 0) {
                        body = decoded;
                    }
                }
                const char *end2 = body + body_len;
                char *ep1 = NULL;
                double va = strtod(body, &ep1);
                if (ep1 && ep1 != body) {
                    s->a = va;
                    const char *p2 = ep1;
                    while (p2 < end2 && (*p2 == ' ' || *p2 == '\t' || *p2 == ',')) p2++;
                    if (p2 < end2) {
                        char *ep2 = NULL;
                        double vb = strtod(p2, &ep2);
                        if (ep2 && ep2 != p2) s->b = vb;
                    }
                }
                free(decoded);
            }
            /* body complete — compute result */
            double result = s->a * s->b;   /* PRODUCT */
            char json[256];
            int jl = snprintf(json, sizeof(json),
                "{\"worker\":\"%s\",\"request_no\":%" PRIu64 ",\"result\":%.6g}\n",
                worker_name, s->req_no, result);
            if (jl <= 0 || (size_t)jl >= sizeof(json)) { session_close(s); return; }
            s->resp_len = (size_t)snprintf(s->resp, sizeof(s->resp),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: %s\r\n"
                "\r\n%s",
                jl,
                s->should_close ? "close" : "keep-alive",
                json);
            s->resp_off = 0;
            s->state    = S_WRITE_RESP;
            epoll_mod(s->fd, EPOLLOUT);
            again = 1;
            break;
        }

        case S_WRITE_RESP: {
            while (s->resp_off < s->resp_len) {
                ssize_t n = write(s->fd,
                                  s->resp + s->resp_off,
                                  s->resp_len - s->resp_off);
                if (n > 0) {
                    s->resp_off += (size_t)n;
                } else if (n == 0) {
                    session_close(s); return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                    session_close(s); return;
                }
            }
            if (s->should_close) { session_close(s); return; }
            session_reset_for_next(s);
            epoll_mod(s->fd, EPOLLIN);
            break;
        }

        } /* end switch */
    } while (again);
}

/* ── Accept new TCP connections ──────────────────────────────────────── */

static void handle_accept(int listen_fd)
{
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        session_t *s = session_new(conn_fd);
        if (!s) continue;
        if (epoll_add(conn_fd, EPOLLIN) < 0) {
            perror("[vanilla-fn] epoll_add");
            s_sessions[conn_fd] = NULL;
            free(s);
            close(conn_fd);
        }
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void)
{
    const char *worker_name = getenv("BENCH2_WORKER_NAME");
    if (!worker_name || worker_name[0] == '\0') worker_name = "timing-fn-b";

    const char *port_str = getenv("BENCH2_LISTEN_PORT");
    if (!port_str || port_str[0] == '\0') port_str = "8080";

    signal(SIGPIPE, SIG_IGN);

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_flags    = AI_PASSIVE;

    struct addrinfo *res = NULL;
    if (getaddrinfo(NULL, port_str, &hints, &res) != 0) {
        fprintf(stderr, "[vanilla-fn] getaddrinfo failed\n");
        return 1;
    }

    int listen_fd = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        listen_fd = socket(it->ai_family,
                           it->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           it->ai_protocol);
        if (listen_fd < 0) continue;
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 &&
            listen(listen_fd, 4096) == 0) break;
        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "[vanilla-fn] bind/listen failed on port %s\n", port_str);
        return 1;
    }

    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) { perror("[vanilla-fn] epoll_create1"); return 1; }

    if (epoll_add(listen_fd, EPOLLIN) < 0) {
        perror("[vanilla-fn] epoll_add listen_fd");
        return 1;
    }

    memset(s_sessions, 0, sizeof(s_sessions));
    fprintf(stderr, "[vanilla-fn] %s listening on :%s [epoll multi-session]\n",
            worker_name, port_str);

    struct epoll_event events[WORKER_MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(s_epoll_fd, events, WORKER_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[vanilla-fn] epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int      efd = events[i].data.fd;
            uint32_t rev = events[i].events;
            if (efd == listen_fd) {
                handle_accept(listen_fd);
                continue;
            }
            session_t *s = (efd >= 0 && efd < MAX_FD) ? s_sessions[efd] : NULL;
            if (!s) {
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, efd, NULL);
                close(efd);
                continue;
            }
            if (rev & (EPOLLHUP | EPOLLERR)) { session_close(s); continue; }
            advance_session(s, worker_name);
        }
    }
    return 0;
}