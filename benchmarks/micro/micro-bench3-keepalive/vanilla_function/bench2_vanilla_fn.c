/*
 * bench2_vanilla_fn.c — Benchmark-2 keepalive vanilla container function.
 *
 * Plain HTTP/1.1 server (no TLS — the bench3 faasd gateway handles TLS
 * termination directly on port 8444).
 *
 * Timing discipline:
 *   - top1 is injected by the direct-HTTPS gateway listener in the
 *     X-Bench2-Top1-Rdtsc header when the gateway begins the first consuming
 *     read for the request.
 *   - top2 is stamped HERE, after reading all body bytes of the request.
 *   - delta_cycles = top2 - top1. With the shared bench2 time source now
 *     using CLOCK_MONOTONIC_RAW, this value is already in nanosecond ticks.
 *
 * Response JSON:
 *   {
 *     "worker":        "bench2-fn-a",
 *     "request_no":   1,
 *     "path":         "vanilla",
 *     "top1_rdtsc":   <uint64>,
 *     "top2_rdtsc":   <uint64>,
 *     "delta_cycles": <uint64>,
 *     "cntfrq":       <uint64>,
 *     "delta_ns":     <uint64>
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>

#include "../common/bench2_rdtsc.h"

/* ─── HTTP parsing helpers ──────────────────────────────────────────────── */

static int header_has_token(const char *hdr, size_t hdr_len,
                            const char *header_name, const char *token)
{
    const char *p = hdr;
    const char *end = hdr + hdr_len;
    size_t header_name_len = strlen(header_name);
    size_t token_len = strlen(token);

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? (nl + 1) : end;
        const char *line = p;

        while (line < line_end && (*line == '\r' || *line == '\n')) {
            line++;
        }

        if ((size_t)(line_end - line) >= header_name_len &&
            strncasecmp(line, header_name, header_name_len) == 0) {
            const char *value = line + header_name_len;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }

            for (const char *cursor = value; cursor + token_len <= line_end; cursor++) {
                if (strncasecmp(cursor, token, token_len) == 0) {
                    return 1;
                }
            }
        }

        p = line_end;
    }

    return 0;
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

static int ensure_capacity(char **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) {
        return 0;
    }

    size_t new_cap = *cap ? *cap : 8192;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) {
            return -1;
        }
        new_cap *= 2;
    }

    char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) {
        return -1;
    }

    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

static int read_more(int fd, char **buf, size_t *len, size_t *cap)
{
    if (ensure_capacity(buf, cap, *len + 4096 + 1) != 0) {
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, *buf + *len, *cap - *len - 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }

        *len += (size_t)n;
        (*buf)[*len] = '\0';
        return 0;
    }
}

static ssize_t find_subseq(const char *buf, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || len < needle_len) {
        return -1;
    }

    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static ssize_t find_line_end(const char *buf, size_t start, size_t len, size_t *eol_len_out)
{
    for (size_t i = start; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start && buf[i - 1] == '\r') {
                *eol_len_out = 2;
                return (ssize_t)(i - 1);
            }
            *eol_len_out = 1;
            return (ssize_t)i;
        }
    }

    return -1;
}

static int consume_chunked_body(int fd, char **buf, size_t *len, size_t *cap,
                                size_t cursor, size_t *body_bytes_read_out)
{
    size_t body_bytes_read = 0;

    for (;;) {
        size_t eol_len = 0;
        ssize_t line_end = -1;
        while ((line_end = find_line_end(*buf, cursor, *len, &eol_len)) < 0) {
            if (read_more(fd, buf, len, cap) != 0) {
                return -1;
            }
        }

        size_t line_len = (size_t)line_end - cursor;
        if (line_len >= 64) {
            return -1;
        }

        char line[64];
        memcpy(line, *buf + cursor, line_len);
        line[line_len] = '\0';

        char *semi = strchr(line, ';');
        if (semi) {
            *semi = '\0';
        }

        errno = 0;
        char *num_end = NULL;
        unsigned long chunk_size = strtoul(line, &num_end, 16);
        if (errno != 0 || !num_end || num_end == line) {
            return -1;
        }

        cursor = (size_t)line_end + eol_len;

        if (chunk_size == 0) {
            for (;;) {
                while ((line_end = find_line_end(*buf, cursor, *len, &eol_len)) < 0) {
                    if (read_more(fd, buf, len, cap) != 0) {
                        return -1;
                    }
                }

                line_len = (size_t)line_end - cursor;
                cursor = (size_t)line_end + eol_len;
                if (line_len == 0) {
                    *body_bytes_read_out = body_bytes_read;
                    return 0;
                }
            }
        }

        while (*len - cursor < chunk_size + 1) {
            if (read_more(fd, buf, len, cap) != 0) {
                return -1;
            }
        }

        body_bytes_read += (size_t)chunk_size;
        cursor += (size_t)chunk_size;

        if (*len - cursor >= 2 && (*buf)[cursor] == '\r' && (*buf)[cursor + 1] == '\n') {
            cursor += 2;
        } else if (*len - cursor >= 1 && (*buf)[cursor] == '\n') {
            cursor += 1;
        } else {
            while (*len - cursor < 2) {
                if (read_more(fd, buf, len, cap) != 0) {
                    return -1;
                }
            }

            if ((*buf)[cursor] == '\r' && (*buf)[cursor + 1] == '\n') {
                cursor += 2;
            } else if ((*buf)[cursor] == '\n') {
                cursor += 1;
            } else {
                return -1;
            }
        }
    }
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

        size_t cap = 65536, len = 0;
        char *buf = malloc(cap);
        if (!buf) break;

        ssize_t hdr_end = -1;
        size_t hdr_sep_len = 0;
        while (hdr_end < 0) {
            if (len >= 1024 * 1024) {
                free(buf);
                goto done;
            }

            if (read_more(conn_fd, &buf, &len, &cap) != 0) {
                free(buf);
                goto done;
            }

            hdr_end = find_subseq(buf, len, "\r\n\r\n");
            if (hdr_end >= 0) {
                hdr_sep_len = 4;
                break;
            }

            hdr_end = find_subseq(buf, len, "\n\n");
            if (hdr_end >= 0) {
                hdr_sep_len = 2;
                break;
            }
        }

        size_t hdr_bytes = (size_t)hdr_end + hdr_sep_len;

        long long top1_ll   = parse_header_int64(buf, hdr_bytes,
                                                  "x-bench2-top1-rdtsc:");
        long long cntfrq_ll = parse_header_int64(buf, hdr_bytes,
                                                  "x-bench2-cntfrq:");

        long long cl = parse_header_int64(buf, hdr_bytes, "content-length:");
        if (cl < 0) cl = 0;
        int is_chunked = header_has_token(buf, hdr_bytes, "transfer-encoding:", "chunked");

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

        size_t body_in = 0;
        if (is_chunked) {
            if (consume_chunked_body(conn_fd, &buf, &len, &cap, hdr_bytes, &body_in) != 0) {
                free(buf);
                goto done;
            }
        } else {
            size_t body_need = (size_t)cl;
            body_in = (len > hdr_bytes) ? (len - hdr_bytes) : 0;
            if (body_in > body_need) {
                body_in = body_need;
            }

            while (body_in < body_need) {
                char scratch[8192];
                size_t want = body_need - body_in;
                if (want > sizeof(scratch)) want = sizeof(scratch);
                ssize_t n = read(conn_fd, scratch, want);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    free(buf);
                    goto done;
                }
                body_in += (size_t)n;
            }
        }

        uint64_t top2  = bench2_rdtsc();
        free(buf);
        uint64_t freq  = bench2_cntfrq();

        uint64_t top1  = (top1_ll   > 0) ? (uint64_t)top1_ll   : 0;
        uint64_t cntfrq = (cntfrq_ll > 0) ? (uint64_t)cntfrq_ll : freq;
        uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;
        uint64_t delta_ns = (cntfrq > 0)
            ? (uint64_t)(((long double)delta * 1000000000.0L) / (long double)cntfrq)
            : 0;

        char json[640];
        int jlen = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"vanilla\""
            ",\"top1_rdtsc\":%" PRIu64
            ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_cycles\":%" PRIu64
            ",\"cntfrq\":%" PRIu64
            ",\"delta_ns\":%" PRIu64
            ",\"body_bytes_read\":%zu"
            ",\"content_length\":%lld}\n",
            worker_name, req_no,
            top1, top2, delta, cntfrq, delta_ns, body_in, cl);
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

        /* Keep the measured path free of per-request fork overhead. */
        handle_connection(conn_fd, worker_name);
    }
}
