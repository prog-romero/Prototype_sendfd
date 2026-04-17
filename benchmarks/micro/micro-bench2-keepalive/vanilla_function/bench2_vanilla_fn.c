/*
 * bench2_vanilla_fn.c — Benchmark-2 keepalive vanilla container function.
 *
 * Plain HTTP/1.1 server (no TLS — faasd gateway handles TLS termination via
 * the bench2 vanilla proxy on port 8444).
 *
 * Timing discipline:
 *   - top1 is injected by the vanilla proxy in the X-Bench2-Top1-Rdtsc header
 *     (bench2_rdtsc() stamped at the proxy just before wolfSSL_read).
 *   - top2 is stamped HERE, after reading all body bytes of the request.
 *   - delta_cycles = top2 - top1 (valid because CNTVCT_EL0 is globally
 *     synchronised across all cores, even across process boundaries).
 *
 * Response JSON:
 *   {
 *     "worker":        "bench2-fn-a",
 *     "request_no":   1,
 *     "path":         "vanilla",
 *     "top1_rdtsc":   <uint64>,
 *     "top2_rdtsc":   <uint64>,
 *     "delta_cycles": <uint64>,
 *     "cntfrq":       <uint64>
 *   }
 *
 * Build (in Dockerfile):
 *   gcc -O2 -Wall -o bench2_vanilla_fn bench2_vanilla_fn.c
 *
 * Environment:
 *   BENCH2_WORKER_NAME   name to return in "worker" field (default: "bench2-fn-?")
 *   BENCH2_LISTEN_PORT   listening port (default: 8080)
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../common/bench2_rdtsc.h"

/* ─── HTTP parsing helpers ──────────────────────────────────────────────── */

static ssize_t find_double_crlf(const char *buf, size_t len)
{
    if (len < 4) return -1;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

/* Case-insensitive scan for "needle:" in HTTP headers. */
static long long parse_header_int64(const char *hdr, size_t hdr_len,
                                    const char *needle)
{
    size_t needle_len = strlen(needle);
    const char *p   = hdr;
    const char *end = hdr + hdr_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len >= needle_len) {
            int match = 1;
            for (size_t k = 0; k < needle_len && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                match = (c == needle[k]);
            }
            if (match) {
                const char *v = p + needle_len;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                char *endp = NULL;
                long long val = strtoll(v, &endp, 10);
                if (endp && endp > v) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* ─── Per-connection handler ─────────────────────────────────────────────── */

static void handle_connection(int conn_fd, const char *worker_name)
{
    uint64_t req_no = 0;

    for (;;) {
        req_no++;

        /* Read headers */
        size_t cap = 65536, len = 0;
        char *buf = malloc(cap);
        if (!buf) break;

        ssize_t hdr_end = -1;
        while (hdr_end < 0) {
            if (len + 4096 > cap) {
                size_t nc = cap * 2;
                char *nb  = realloc(buf, nc);
                if (!nb) { free(buf); goto done; }
                buf = nb; cap = nc;
            }
            ssize_t n = read(conn_fd, buf + len, cap - len - 1);
            if (n <= 0) { free(buf); goto done; }
            len     += (size_t)n;
            hdr_end  = find_double_crlf(buf, len);
        }

        size_t hdr_bytes = (size_t)hdr_end + 4;

        /* Parse X-Bench2-Top1-Rdtsc and X-Bench2-Cntfrq */
        long long top1_ll   = parse_header_int64(buf, hdr_bytes,
                                                  "x-bench2-top1-rdtsc:");
        long long cntfrq_ll = parse_header_int64(buf, hdr_bytes,
                                                  "x-bench2-cntfrq:");

        /* Parse Content-Length */
        long long cl = parse_header_int64(buf, hdr_bytes, "content-length:");
        if (cl < 0) cl = 0;

        /* Check Connection: close */
        int should_close = 0;
        {
            const char *p   = buf;
            const char *end = buf + hdr_bytes;
            while (p < end) {
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
                const char key[] = "connection:";
                if (ll >= sizeof(key) - 1) {
                    int m = 1;
                    for (size_t k = 0; k < sizeof(key) - 1 && m; k++) {
                        char c = p[k];
                        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                        m = (c == key[k]);
                    }
                    if (m) {
                        const char *v = p + sizeof(key) - 1;
                        const char *ve = nl ? nl : end;
                        while (v + 4 < ve) {
                            if ((v[0]=='c'||v[0]=='C') && (v[1]=='l'||v[1]=='L') &&
                                (v[2]=='o'||v[2]=='O') && (v[3]=='s'||v[3]=='S') &&
                                (v[4]=='e'||v[4]=='E'))
                            { should_close = 1; break; }
                            v++;
                        }
                    }
                }
                p = nl ? nl + 1 : end;
            }
        }

        /* Read rest of body */
        size_t body_in  = (len > hdr_bytes) ? len - hdr_bytes : 0;
        size_t body_need = (size_t)cl;
        while (body_in < body_need) {
            char scratch[8192];
            size_t want = body_need - body_in;
            if (want > sizeof(scratch)) want = sizeof(scratch);
            ssize_t n = read(conn_fd, scratch, want);
            if (n <= 0) break;
            body_in += (size_t)n;
        }
        free(buf);

        /* ── top2 stamped after full body read ── */
        uint64_t top2  = bench2_rdtsc();
        uint64_t freq  = bench2_cntfrq();

        uint64_t top1  = (top1_ll   > 0) ? (uint64_t)top1_ll   : 0;
        uint64_t cntfrq = (cntfrq_ll > 0) ? (uint64_t)cntfrq_ll : freq;
        uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;

        /* Build JSON response */
        char json[512];
        int jlen = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"vanilla\""
            ",\"top1_rdtsc\":%" PRIu64
            ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_cycles\":%" PRIu64
            ",\"cntfrq\":%" PRIu64 "}\n",
            worker_name, req_no,
            top1, top2, delta, cntfrq);
        if (jlen <= 0 || (size_t)jlen >= sizeof(json)) jlen = 0;

        char resp[768];
        int rlen = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "\r\n"
            "%s",
            jlen,
            should_close ? "close" : "keep-alive",
            jlen > 0 ? json : "");

        if (rlen <= 0 || (size_t)rlen >= sizeof(resp)) break;
        if (write_all(conn_fd, resp, (size_t)rlen) != 0) break;
        if (should_close) break;
    }

done:
    close(conn_fd);
}

/* ─── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    const char *worker_name = getenv("BENCH2_WORKER_NAME");
    if (!worker_name || worker_name[0] == '\0')
        worker_name = "bench2-fn-?";

    const char *port_str = getenv("BENCH2_LISTEN_PORT");
    if (!port_str || port_str[0] == '\0')
        port_str = "8080";

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_flags    = AI_PASSIVE;
    struct addrinfo *res = NULL;
    if (getaddrinfo(NULL, port_str, &hints, &res) != 0) {
        fprintf(stderr, "[bench2-fn] getaddrinfo failed\n");
        return 1;
    }

    int listen_fd = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        listen_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listen_fd < 0) continue;
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 &&
            listen(listen_fd, 128) == 0)
            break;
        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "[bench2-fn] bind/listen on port %s failed\n", port_str);
        return 1;
    }

    fprintf(stderr, "[bench2-fn] %s listening on port %s\n", worker_name, port_str);

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("[bench2-fn] accept");
            continue;
        }
        pid_t pid = fork();
        if (pid < 0) { close(conn_fd); continue; }
        if (pid == 0) {
            close(listen_fd);
            handle_connection(conn_fd, worker_name);
            exit(0);
        }
        close(conn_fd);
    }
}
