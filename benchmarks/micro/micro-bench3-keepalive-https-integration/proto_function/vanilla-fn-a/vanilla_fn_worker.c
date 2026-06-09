/*
 * vanilla_fn_worker.c — Vanilla timing function worker
 *
 * A simple persistent HTTP/1.1 server that listens on :8080 and is designed
 * to run as the "fprocess" under the of-watchdog in HTTP mode.
 *
 * Timing convention (identical to the prototype integration worker):
 *   top1_rdtsc  = value of the "X-Top1-Rdtsc" request header (nanoseconds,
 *                 CLOCK_MONOTONIC_RAW stamped by the gateway observer goroutine
 *                 just before the first byte was read from the client socket).
 *   top2_rdtsc  = CLOCK_MONOTONIC_RAW stamped HERE after the last byte of the
 *                 request body has been read from the watchdog's TCP socket.
 *   delta_ns    = top2 - top1  (nanoseconds)
 *   cntfrq      = 1_000_000_000 (ns reference)
 *
 * This measures the latency of the full OpenFaaS vanilla proxy path:
 *   data arrives at gateway kernel buffer  →  function finishes reading body
 *
 * Environment variables:
 *   VANILLA_FN_NAME   this function's name reported in JSON (default "vanilla-fn-a")
 *   VANILLA_FN_PORT   listening port (default "8080")
 *
 * JSON response (same schema as timing_fn_ka_worker):
 *   {
 *     "worker":          "<VANILLA_FN_NAME>",
 *     "request_no":      <uint64>,
 *     "path":            "vanilla-proxy",
 *     "top1_rdtsc":      <uint64 ns>,
 *     "top2_rdtsc":      <uint64 ns>,
 *     "delta_cycles":    <uint64>  (= delta_ns, units already ns),
 *     "cntfrq":          1000000000,
 *     "delta_ns":        <uint64 ns>,
 *     "body_bytes_read": <size_t>,
 *     "content_length":  <long long>
 *   }
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
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

/* Case-insensitive header value extraction.
 * Returns value start pointer (pointing into buf) and writes length to *vlen.
 * Returns NULL if header not found. */
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

static uint64_t parse_top1_header(const char *headers, size_t hlen)
{
    size_t vlen = 0;
    const char *v = extract_header(headers, hlen, "X-Top1-Rdtsc", &vlen);
    if (!v) return 0;
    char tmp[32];
    size_t copy = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
    memcpy(tmp, v, copy);
    tmp[copy] = '\0';
    return (uint64_t)strtoull(tmp, NULL, 10);
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

/* ── Per-connection session loop ─────────────────────────────────────────── */

static void handle_connection(int cfd, const char *fn_name)
{
    static uint64_t global_req_no = 0;

    char *buf = NULL;
    size_t cap = 0;

    for (;;) {
        /* ── Receive HTTP headers ────────────────────────────────────────── */
        size_t total = 0;
        ssize_t hdr_end = -1;
        size_t hdr_sep  = 0;

        while (hdr_end < 0) {
            if (cap < total + 8192 + 1) {
                size_t nc = cap ? cap * 2 : 65536;
                char *nb  = realloc(buf, nc);
                if (!nb) goto done;
                buf = nb;
                cap = nc;
            }
            ssize_t n = recv(cfd, buf + total, cap - total - 1, 0);
            if (n <= 0) goto done;
            total += (size_t)n;
            buf[total] = '\0';

            ssize_t e4 = find_seq(buf, total, "\r\n\r\n");
            if (e4 >= 0) { hdr_end = e4; hdr_sep = 4; break; }
            ssize_t e2 = find_seq(buf, total, "\n\n");
            if (e2 >= 0) { hdr_end = e2; hdr_sep = 2; break; }
        }

        size_t hdr_sz = (size_t)hdr_end + hdr_sep;
        long long cl  = parse_content_length(buf, hdr_sz);
        if (cl < 0) cl = 0;

        /* Extract top1 from the request header injected by the gateway. */
        uint64_t top1 = parse_top1_header(buf, hdr_sz);

        /* ── Receive body ────────────────────────────────────────────────── */
        size_t body_read = (total > hdr_sz) ? (total - hdr_sz) : 0;
        while (body_read < (size_t)cl) {
            char scratch[65536];
            size_t want = (size_t)cl - body_read;
            if (want > sizeof(scratch)) want = sizeof(scratch);
            ssize_t n = recv(cfd, scratch, want, 0);
            if (n <= 0) goto done;
            body_read += (size_t)n;
        }

        /* ── Stamp top2 immediately after last byte of body is read ─────── */
        uint64_t top2    = get_ns();
        uint64_t delta   = (top2 > top1 && top1 > 0) ? (top2 - top1) : 0;
        uint64_t req_no  = __atomic_add_fetch(&global_req_no, 1, __ATOMIC_RELAXED);
        int close_after  = should_close(buf, hdr_sz);

        /* ── Build JSON response ─────────────────────────────────────────── */
        char json[512];
        int jl = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"vanilla-proxy\""
            ",\"top1_rdtsc\":%" PRIu64
            ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_cycles\":%" PRIu64
            ",\"cntfrq\":%" PRIu64
            ",\"delta_ns\":%" PRIu64
            ",\"body_bytes_read\":%zu"
            ",\"content_length\":%lld}",
            fn_name, req_no,
            top1, top2, delta, CNTFRQ, delta,
            body_read, cl);

        char resp[768];
        int rl = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "\r\n%s",
            jl,
            close_after ? "close" : "keep-alive",
            json);

        if (write(cfd, resp, (size_t)rl) < 0) goto done;
        if (close_after) goto done;

        /* Reset for next keepalive request on this socket. */
        total = 0;
    }

done:
    free(buf);
    close(cfd);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name  = getenv("VANILLA_FN_NAME");
    const char *port_str = getenv("VANILLA_FN_PORT");
    if (!fn_name  || !fn_name[0])  fn_name  = "vanilla-fn-a";
    if (!port_str || !port_str[0]) port_str = "8080";

    int port = atoi(port_str);
    if (port <= 0) port = 8080;

    fprintf(stderr, "[vanilla-fn] %s starting on port %d\n", fn_name, port);

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

    fprintf(stderr, "[vanilla-fn] %s listening on :%d\n", fn_name, port);

    for (;;) {
        int cfd = accept(listenfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        /* For simplicity, handle each connection synchronously.
         * The watchdog creates a new connection per request in http mode,
         * so a single-threaded accept loop is sufficient for benchmarking. */
        handle_connection(cfd, fn_name);
    }
}
