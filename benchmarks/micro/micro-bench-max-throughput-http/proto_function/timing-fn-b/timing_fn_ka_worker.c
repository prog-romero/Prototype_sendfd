/*
 * timing_fn_ka_worker.c — Keep-Alive HTTP prototype worker
 * Function B: computes the PRODUCT of two numbers from the request body.
 *
 * Receives an HTTP connection FD from the gateway via Unix socket.
 * Serves requests in a keep-alive loop:
 *   - If the next request targets THIS function → serve it, loop
 *   - If the next request targets ANOTHER function → relay FD back to the gateway
 *
 * Environment variables:
 *   HTTPMIGRATE_KA_FUNCTION_NAME   this function's name (e.g. "timing-fn-a")
 *   HTTPMIGRATE_KA_SOCKET_DIR      shared socket directory (default /run/httpmigrate)
 *   HTTPMIGRATE_KA_RELAY_SOCKET    gateway relay socket path (default /run/httpmigrate/relay.sock)
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sendfd.h"
#include "unix_socket.h"

/* ── Payload (must match gateway) ───────────────────────────────────────── */

#define HTTPMIGRATE_MAGIC      0x484D4B41U   /* 'HMKA' */
#define HTTPMIGRATE_VERSION    2U
#define HTTPMIGRATE_TARGET_LEN 128

typedef struct {
    uint32_t magic;
    uint32_t version;
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} httpmigrate_ka_payload_t;

/* ── HTTP parsing helpers ────────────────────────────────────────────────── */

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle) {
    const size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static long long parse_content_length(const char *headers, size_t hlen) {
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

/*
 * Parse /function/<name> from the first line of an HTTP request.
 * buf must be null-terminated.
 */
static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *out, size_t outsz) {
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
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') { nlen = i; break; }
    if (!nlen || nlen + 1 > outsz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

/* ── Session state machine ──────────────────────────────────────────────── */

typedef enum {
    S_READ_HEADERS,  /* accumulate request until \r\n\r\n                 */
    S_READ_BODY,     /* drain remaining body bytes                        */
    S_WRITE_RESP,    /* flush HTTP response                               */
    S_PEEK_OWNER,    /* peek next request; relay fd if wrong owner        */
} sess_state_t;

typedef struct session {
    int          fd;
    uint64_t     req_no;
    sess_state_t state;
    /* read accumulation buffer */
    unsigned char *buf;
    size_t         cap;
    size_t         len;
    /* header-parsing results */
    ssize_t    hdr_end;
    size_t     hdr_sep;
    long long  content_length;
    size_t     body_in;
    int        should_close;
    double     a, b;
    /* response write buffer */
    char   resp[768];
    size_t resp_len;
    size_t resp_off;
} session_t;

/* ── Global epoll + session table ───────────────────────────────────────── */

#define MAX_FD            65536
#define WORKER_MAX_EVENTS 64

static int        s_epoll_fd        = -1;
static session_t *s_sessions[MAX_FD];   /* NULL = unused */
static uint8_t    s_pending_uds[MAX_FD];

/* ── Low-level helpers ───────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int epoll_add(int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod(int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

/* ── Session lifecycle ───────────────────────────────────────────────────── */

static void session_close(session_t *s) {
    if (!s) return;
    if (s->fd >= 0 && s->fd < MAX_FD) s_sessions[s->fd] = NULL;
    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
    close(s->fd);
    free(s->buf);
    free(s);
}

static session_t *session_new(int fd) {
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

static void session_reset_for_next(session_t *s) {
    s->len            = 0;
    s->hdr_end        = -1;
    s->hdr_sep        = 0;
    s->content_length = 0;
    s->body_in        = 0;
    s->should_close   = 0;
    s->a              = 1.0;
    s->b              = 2.0;
    s->resp_len       = 0;
    s->resp_off       = 0;
    s->req_no++;
    s->state          = S_PEEK_OWNER;
}

static void add_client_session_from_payload(int client_fd,
                                            const httpmigrate_ka_payload_t *payload)
{
    if (client_fd < 0) return;
    if (!payload || payload->magic != HTTPMIGRATE_MAGIC ||
        payload->version != HTTPMIGRATE_VERSION) {
        fprintf(stderr, "[ka-worker] bad magic/version — dropping fd\n");
        close(client_fd);
        return;
    }

    if (set_nonblocking(client_fd) < 0) {
        perror("[ka-worker] set_nonblocking");
        close(client_fd);
        return;
    }

    session_t *s = session_new(client_fd);
    if (!s) return;
    if (epoll_add(client_fd, EPOLLIN) < 0) {
        perror("[ka-worker] epoll_add client_fd");
        s_sessions[client_fd] = NULL;
        free(s);
        close(client_fd);
    }
}

static void handle_pending_uds_fd(int conn_fd)
{
    httpmigrate_ka_payload_t payload;
    int client_fd = -1;

    memset(&payload, 0, sizeof(payload));
    if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, conn_fd, NULL);
        if (conn_fd >= 0 && conn_fd < MAX_FD) s_pending_uds[conn_fd] = 0;
        close(conn_fd);
        if (client_fd >= 0) close(client_fd);
        return;
    }

    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, conn_fd, NULL);
    if (conn_fd >= 0 && conn_fd < MAX_FD) s_pending_uds[conn_fd] = 0;
    close(conn_fd);
    add_client_session_from_payload(client_fd, &payload);
}

/* ── UDS handoff: drain all pending FD deliveries from the gateway ───────── */

static void handle_listen_fd(int listen_fd) {
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        if (conn_fd >= MAX_FD) {
            close(conn_fd);
            continue;
        }

        s_pending_uds[conn_fd] = 1;
        if (epoll_add(conn_fd, EPOLLIN) < 0) {
            s_pending_uds[conn_fd] = 0;
            close(conn_fd);
            continue;
        }

        handle_pending_uds_fd(conn_fd);
    }
}

/* ── Per-session non-blocking state machine ─────────────────────────────── */

static void advance_session(session_t *s,
                            const char *fn_name,
                            const char *relay_socket)
{
    int again;
    do {
        again = 0;
        switch (s->state) {

        /* ── Peek next request; relay if wrong owner ──── */
        case S_PEEK_OWNER: {
            unsigned char peek_buf[1024];
            ssize_t pn = recv(s->fd, peek_buf, sizeof(peek_buf) - 1,
                              MSG_PEEK | MSG_DONTWAIT);
            if (pn == 0) { session_close(s); return; }
            if (pn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                session_close(s); return;
            }
            peek_buf[pn] = '\0';
            char owner[HTTPMIGRATE_TARGET_LEN];
            if (!parse_request_owner(peek_buf, (size_t)pn, owner, sizeof(owner))) {
                session_close(s); return;
            }
            if (strcmp(owner, fn_name) != 0) {
                /* wrong owner — relay fd to gateway, free this session */
                httpmigrate_ka_payload_t rp;
                memset(&rp, 0, sizeof(rp));
                rp.magic   = HTTPMIGRATE_MAGIC;
                rp.version = HTTPMIGRATE_VERSION;
                snprintf(rp.target_function, sizeof(rp.target_function),
                         "%s", owner);
                /* de-register before sendfd_with_state (which closes our fd) */
                int fd_to_relay = s->fd;
                s_sessions[fd_to_relay] = NULL;
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd_to_relay, NULL);
                free(s->buf);
                free(s);
                int relay_fd = unix_client_connect(relay_socket);
                if (relay_fd >= 0) {
                    sendfd_with_state(relay_fd, fd_to_relay, &rp, sizeof(rp));
                    close(relay_fd);
                } else {
                    close(fd_to_relay);
                }
                return;
            }
            /* correct owner: data already available — read headers next */
            s->state = S_READ_HEADERS;
            again    = 1;
            break;
        }

        /* ── Accumulate until end-of-headers ─────────── */
        case S_READ_HEADERS: {
            int found = 0;
            for (;;) {
                if (s->cap < s->len + 4096 + 1) {
                    size_t nc = s->cap ? s->cap * 2 : 16384;
                    unsigned char *nb = realloc(s->buf, nc);
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
            if (!found) break;  /* wait for more data */
            size_t hdr_sz = (size_t)s->hdr_end + s->hdr_sep;
            s->content_length = parse_content_length((const char *)s->buf, hdr_sz);
            if (s->content_length < 0) s->content_length = 0;
            s->should_close = (
                find_subseq(s->buf, hdr_sz, "Connection: close") >= 0 ||
                find_subseq(s->buf, hdr_sz, "connection: close") >= 0
            );
            s->body_in = (s->len > hdr_sz) ? (s->len - hdr_sz) : 0;
            if (s->body_in > 0) {
                char *ep1 = NULL;
                double va = strtod((const char *)(s->buf + hdr_sz), &ep1);
                if (ep1 && ep1 != (const char *)(s->buf + hdr_sz)) {
                    s->a = va;
                    const char *p2   = ep1;
                    const char *end2 = (const char *)(s->buf + hdr_sz) + s->body_in;
                    while (p2 < end2 && (*p2 == ' ' || *p2 == '\t' || *p2 == ',')) p2++;
                    if (p2 < end2) {
                        char *ep2 = NULL;
                        double vb = strtod(p2, &ep2);
                        if (ep2 && ep2 != p2) s->b = vb;
                    }
                }
            }
            s->state = S_READ_BODY;
            again    = 1;
            break;
        }

        /* ── Drain remaining body bytes ────────────────── */
        case S_READ_BODY: {
            while (s->body_in < (size_t)s->content_length) {
                unsigned char scratch[16384];
                size_t want = (size_t)s->content_length - s->body_in;
                if (want > sizeof(scratch)) want = sizeof(scratch);
                ssize_t n = recv(s->fd, scratch, want, MSG_DONTWAIT);
                if (n > 0) {
                    s->body_in += (size_t)n;
                } else if (n == 0) {
                    session_close(s); return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                    session_close(s); return;
                }
            }
            /* body complete — compute result */
            double result = s->a * s->b;   /* PRODUCT */
            char json[768];
            int jl = snprintf(json, sizeof(json),
                "{\"worker\":\"%s\",\"request_no\":%" PRIu64 ",\"result\":%.6g,\"_pad\":\"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\"}",
                fn_name, s->req_no, result);
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
            epoll_mod(s->fd, EPOLLOUT);  /* switch to write-ready watch */
            again = 1;
            break;
        }

        /* ── Flush HTTP response ───────────────────────── */
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
            epoll_mod(s->fd, EPOLLIN);  /* back to read-ready watch */
            /* no again=1: must wait for new request bytes */
            break;
        }

        } /* end switch */
    } while (again);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name      = getenv("HTTPMIGRATE_KA_FUNCTION_NAME");
    const char *socket_dir   = getenv("HTTPMIGRATE_KA_SOCKET_DIR");
    const char *relay_socket = getenv("HTTPMIGRATE_KA_RELAY_SOCKET");

    if (!fn_name      || !fn_name[0])      fn_name      = "timing-fn-b";
    if (!socket_dir   || !socket_dir[0])   socket_dir   = "/run/httpmigrate";
    if (!relay_socket || !relay_socket[0]) relay_socket = "/run/httpmigrate/relay.sock";

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, fn_name);

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "[ka-worker] warning: mkdir %s: %s\n",
                socket_dir, strerror(errno));
    else
        chmod(socket_dir, 0777);

    unlink(sock_path);
    int listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[ka-worker] unix_server_socket(%s) failed: %s\n",
                sock_path, strerror(errno));
        return 1;
    }
    chmod(sock_path, 0777);
    set_nonblocking(listen_fd);   /* required for drain-accept loop */

    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) { perror("[ka-worker] epoll_create1"); return 1; }

    if (epoll_add(listen_fd, EPOLLIN) < 0) {
        perror("[ka-worker] epoll_add listen_fd");
        return 1;
    }

    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_pending_uds, 0, sizeof(s_pending_uds));
    fprintf(stderr, "[ka-worker] %s listening on %s, relay=%s [epoll multi-session]\n",
            fn_name, sock_path, relay_socket);

    struct epoll_event events[WORKER_MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(s_epoll_fd, events, WORKER_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[ka-worker] epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int      efd = events[i].data.fd;
            uint32_t rev = events[i].events;
            if (efd == listen_fd) {
                handle_listen_fd(listen_fd);
                continue;
            }

            if (efd >= 0 && efd < MAX_FD && s_pending_uds[efd]) {
                if (rev & (EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, efd, NULL);
                    s_pending_uds[efd] = 0;
                    close(efd);
                    continue;
                }
                handle_pending_uds_fd(efd);
                continue;
            }

            session_t *s = (efd >= 0 && efd < MAX_FD) ? s_sessions[efd] : NULL;
            if (!s) {
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, efd, NULL);
                close(efd);
                continue;
            }
            if (rev & (EPOLLHUP | EPOLLERR)) { session_close(s); continue; }
            advance_session(s, fn_name, relay_socket);
        }
    }
    return 0;
}
