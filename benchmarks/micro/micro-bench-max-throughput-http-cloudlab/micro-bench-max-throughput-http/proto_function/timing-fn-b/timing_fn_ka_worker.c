/*
 * timing_fn_ka_worker.c — Keep-Alive HTTP prototype worker
 *
 * Receives an HTTP connection FD from the gateway via Unix socket.
 * Serves requests in a keep-alive loop:
 *   - If the next request targets THIS function → serve it, loop
 *   - If the next request targets ANOTHER function → relay FD back to
 *     the gateway relay socket with top1 timing and target_function set
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
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sendfd.h"
#include "unix_socket.h"
#include "bench2_rdtsc.h"

/* ── Payload (must match gateway) ───────────────────────────────────────── */

#define HTTPMIGRATE_MAGIC   0x484D4B41U   /* 'HMKA' */
#define HTTPMIGRATE_VERSION 2U
#define HTTPMIGRATE_TARGET_LEN 128

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t  top1_set;
    uint8_t  _pad[7];
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

/* Wait until TCP fd has incoming data (or peer closes). Returns 1=data, 0=closed, -1=error */
static int wait_readable(int fd) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
#ifdef POLLRDHUP
    pfd.events |= POLLRDHUP;
#endif
    for (;;) {
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        int avail = 0;
        if (ioctl(fd, FIONREAD, &avail) == 0 && avail > 0) return 1;
        if (pfd.revents & (POLLHUP | POLLERR
#ifdef POLLRDHUP
                           | POLLRDHUP
#endif
                          )) return 0;
        /* Peek 1 byte to distinguish data vs close */
        unsigned char byte;
        ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) return 1;
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return -1;
    }
}

/* ── Session loop ────────────────────────────────────────────────────────── */

static void process_session(int client_fd,
                            httpmigrate_ka_payload_t *payload,
                            const char *function_name,
                            const char *relay_socket)
{
    uint64_t req_no = 0;
    bool first_request = true;

    /* Buffer for reading requests */
    unsigned char *buf = NULL;
    size_t cap = 0;

    for (;;) {
        req_no++;

        uint64_t top1, cntfrq_stamp;

        if (first_request && payload->top1_set) {
            /* top1 was stamped at the gateway before the peek */
            top1         = payload->top1_rdtsc;
            cntfrq_stamp = payload->cntfrq;
            first_request = false;
        } else {
            /* Wait for next request on the persistent connection */
            int wrc = wait_readable(client_fd);
            if (wrc <= 0) goto done; /* peer closed or error */

            /* Stamp top1 before peek */
            top1         = bench2_rdtsc();
            cntfrq_stamp = bench2_cntfrq();

            /* Peek request line to determine owner */
            unsigned char peek_buf[1024];
            ssize_t pn = recv(client_fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
            if (pn <= 0) goto done;
            peek_buf[pn] = '\0';

            char owner[HTTPMIGRATE_TARGET_LEN];
            if (!parse_request_owner(peek_buf, (size_t)pn, owner, sizeof(owner))) {
                /* Cannot parse owner — close connection */
                goto done;
            }

            if (strcmp(owner, function_name) != 0) {
                /*
                 * Sub-case B: wrong owner.
                 * Relay the FD + top1 + target back to the gateway.
                 */
                httpmigrate_ka_payload_t relay_payload;
                memset(&relay_payload, 0, sizeof(relay_payload));
                relay_payload.magic      = HTTPMIGRATE_MAGIC;
                relay_payload.version    = HTTPMIGRATE_VERSION;
                relay_payload.top1_rdtsc = top1;
                relay_payload.cntfrq     = cntfrq_stamp;
                relay_payload.top1_set   = 1;
                snprintf(relay_payload.target_function,
                         sizeof(relay_payload.target_function), "%s", owner);

                int relay_fd = unix_client_connect(relay_socket);
                if (relay_fd >= 0) {
                    sendfd_with_state(relay_fd, client_fd, &relay_payload, sizeof(relay_payload));
                    close(relay_fd);
                } else {
                    close(client_fd);
                }
                /* Our session with this fd ends here */
                if (buf) free(buf);
                return;
            }
        }

        /* ── Sub-case A (or first request): WE are the owner — read it ── */

        /* Grow buffer if needed */
        size_t total = 0;
        ssize_t hdr_end = -1;
        size_t hdr_sep = 0;

        while (hdr_end < 0) {
            if (cap < total + 4096 + 1) {
                size_t nc = cap ? cap * 2 : 16384;
                unsigned char *nb = realloc(buf, nc);
                if (!nb) goto done;
                buf = nb; cap = nc;
            }
            ssize_t n = recv(client_fd, buf + total, cap - total - 1, 0);
            if (n <= 0) goto done;
            total += (size_t)n;
            buf[total] = '\0';

            ssize_t e4 = find_subseq(buf, total, "\r\n\r\n");
            if (e4 >= 0) { hdr_end = e4; hdr_sep = 4; break; }
            ssize_t e2 = find_subseq(buf, total, "\n\n");
            if (e2 >= 0) { hdr_end = e2; hdr_sep = 2; break; }
        }

        size_t hdr_sz = (size_t)hdr_end + hdr_sep;
        long long cl = parse_content_length((const char *)buf, hdr_sz);
        if (cl < 0) cl = 0;

        /* Check Connection: close */
        int should_close = 0;
        if (find_subseq(buf, hdr_sz, "Connection: close") >= 0 ||
            find_subseq(buf, hdr_sz, "connection: close") >= 0)
            should_close = 1;

        /* Drain remaining body */
        size_t body_read = (total > hdr_sz) ? (total - hdr_sz) : 0;
        while (body_read < (size_t)cl) {
            unsigned char scratch[16384];
            size_t want = (size_t)cl - body_read;
            if (want > sizeof(scratch)) want = sizeof(scratch);
            ssize_t n = recv(client_fd, scratch, want, 0);
            if (n <= 0) goto done;
            body_read += (size_t)n;
        }

        /* Stamp top2 */
        uint64_t top2  = bench2_rdtsc();
        uint64_t freq  = cntfrq_stamp ? cntfrq_stamp : bench2_cntfrq();
        uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;
        uint64_t delta_ns = freq ? (uint64_t)(((long double)delta * 1e9L) / (long double)freq) : 0;

        /* Build JSON response */
        char json[512];
        int jl = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"prototype-http-ka\""
            ",\"top1_rdtsc\":%" PRIu64
            ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_cycles\":%" PRIu64
            ",\"cntfrq\":%" PRIu64
            ",\"delta_ns\":%" PRIu64
            ",\"body_bytes_read\":%zu"
            ",\"content_length\":%lld}",
            function_name, req_no,
            top1, top2, delta, freq, delta_ns,
            body_read, cl);

        char resp[768];
        int rl = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "\r\n%s",
            jl,
            should_close ? "close" : "keep-alive",
            json);

        if (write(client_fd, resp, (size_t)rl) < 0) goto done;
        if (should_close) goto done;
    }

done:
    if (buf) free(buf);
    close(client_fd);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name      = getenv("HTTPMIGRATE_KA_FUNCTION_NAME");
    const char *socket_dir   = getenv("HTTPMIGRATE_KA_SOCKET_DIR");
    const char *relay_socket = getenv("HTTPMIGRATE_KA_RELAY_SOCKET");

    if (!fn_name      || !fn_name[0])      fn_name      = "timing-fn-a";
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

    fprintf(stderr, "[ka-worker] %s listening on %s, relay=%s\n",
            fn_name, sock_path, relay_socket);

    for (;;) {
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) continue;

        httpmigrate_ka_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        int client_fd = -1;

        if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
            close(conn_fd);
            continue;
        }
        close(conn_fd);

        if (client_fd < 0) continue;
        if (payload.magic != HTTPMIGRATE_MAGIC ||
            payload.version != HTTPMIGRATE_VERSION) {
            fprintf(stderr, "[ka-worker] bad magic 0x%08x\n", payload.magic);
            close(client_fd);
            continue;
        }

        /* Make fd blocking */
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

        process_session(client_fd, &payload, fn_name, relay_socket);
    }
}
