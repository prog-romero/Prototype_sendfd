/*
 * bench2_vanilla_proxy.c — Forward-only vanilla benchmark TLS front-end.
 *
 * Listens on :8444 (TLS, wolfSSL). For each persistent TLS connection from
 * the client it decrypts one HTTP request at a time, forwards it to the local
 * gateway path, then computes the benchmark delta with this timing discipline:
 *
 *   bench2_wait_readable(tcp_fd)   // spin until encrypted bytes arrive
 *   wolfSSL_read(ssl, ...)         // TLS decrypt → full HTTP request
 *   forward request to gateway (:8080)
 *   top1 = bench2_rdtsc()          // stamp after the request is fully written
 *   read upstream response headers // contains top2 stamped by the function
 *   delta = top2 - top1
 *   return benchmark JSON over TLS
 *
 * This keeps TLS decryption outside the timed region while still reusing the
 * existing TLS front-end and function container path.
 *
 * Build (on Raspberry Pi):
 *   gcc -O2 -Wall -o bench2_vanilla_proxy bench2_vanilla_proxy.c \
 *       -I/usr/local/include -L/usr/local/lib -lwolfssl -lpthread
 *
 * Usage:
 *   ./bench2_vanilla_proxy [--listen HOST:PORT] [--upstream HOST:PORT]
 *                          [--cert CERT.pem] [--key KEY.pem]
 *
 * Defaults:
 *   --listen    0.0.0.0:8444
 *   --upstream  127.0.0.1:8080
 *   --cert      /certs/server.crt
 *   --key       /certs/server.key
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>
#include <unistd.h>

#include "../common/bench2_rdtsc.h"

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int wolfssl_write_all(WOLFSSL *ssl, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = wolfSSL_write(ssl, p, (int)len);
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
                continue;
            return -1;
        }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* find "\r\n\r\n" in buf; returns index of the first '\r', or -1. */
static ssize_t find_double_crlf(const char *buf, size_t len)
{
    if (len < 4) return -1;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

/* Case-insensitive integer header search; returns -1 if not found. */
static long long parse_header_int64_ci(const char *hdr, size_t hdr_len,
                                       const char *needle)
{
    const char *p   = hdr;
    const char *end = hdr + hdr_len;
    size_t needle_len = strlen(needle);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len >= needle_len) {
            bool match = true;
            for (size_t k = 0; k < needle_len && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                match = (c == needle[k]);
            }
            if (match) {
                const char *v = p + needle_len;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                long long val = strtoll(v, NULL, 10);
                if (val >= 0) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return -1;
}

static int parse_header_string_ci(const char *hdr, size_t hdr_len,
                                  const char *needle,
                                  char *out, size_t out_sz)
{
    const char *p = hdr;
    const char *end = hdr + hdr_len;
    size_t needle_len = strlen(needle);

    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len >= needle_len) {
            bool match = true;
            for (size_t k = 0; k < needle_len && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                match = (c == needle[k]);
            }
            if (match) {
                const char *v = p + needle_len;
                const char *vend = nl ? nl : end;
                while (v < vend && (*v == ' ' || *v == '\t')) v++;
                while (vend > v && (vend[-1] == '\r' || vend[-1] == ' ' || vend[-1] == '\t')) vend--;
                size_t copy_len = (size_t)(vend - v);
                if (copy_len + 1 > out_sz) return -1;
                memcpy(out, v, copy_len);
                out[copy_len] = '\0';
                return 0;
            }
        }
        p = nl ? nl + 1 : end;
    }

    return -1;
}

/* Case-insensitive header value search; returns -1 if not found. */
static long parse_content_length(const char *hdr, size_t hdr_len)
{
    return (long)parse_header_int64_ci(hdr, hdr_len, "content-length:");
}

/* Return true if the header block contains "connection: close" */
static bool has_connection_close(const char *hdr, size_t hdr_len)
{
    const char *p   = hdr;
    const char *end = hdr + hdr_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char key[] = "connection:";
        if (line_len >= sizeof(key) - 1) {
            bool match = true;
            for (size_t k = 0; k < sizeof(key) - 1 && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                match = (c == key[k]);
            }
            if (match) {
                /* look for "close" in the value */
                const char *v = p + sizeof(key) - 1;
                const char *vend = nl ? nl : end;
                while (v + 4 < vend) {
                    if ((v[0]=='c'||v[0]=='C') && (v[1]=='l'||v[1]=='L') &&
                        (v[2]=='o'||v[2]=='O') && (v[3]=='s'||v[3]=='S') &&
                        (v[4]=='e'||v[4]=='E'))
                        return true;
                    v++;
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    return false;
}

static int connect_tcp(const char *host, const char *port)
{
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ─── Per-connection handler ─────────────────────────────────────────────── */

/*
 * handle_connection — run inside a forked child.
 *
 * Performs the TLS handshake, then loops over HTTP/1.1 keepalive requests:
 *   1. Wait for encrypted data (FIONREAD on raw TCP fd).
 *   2. Read the full HTTP request via wolfSSL_read.
 *   3. Forward the full plaintext request to the gateway.
 *   4. Stamp top1 = bench2_rdtsc() after the write completes.
 *   5. Read top2 metadata from the function response headers.
 *   6. Return the benchmark JSON to the TLS client.
 */
static void handle_connection(int client_fd, WOLFSSL_CTX *ctx,
                              const char *gw_host, const char *gw_port)
{
    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (!ssl) { close(client_fd); return; }

    wolfSSL_set_fd(ssl, client_fd);
    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }

    int tcp_fd = wolfSSL_get_fd(ssl);   /* raw kernel socket for FIONREAD */

    /* ── keepalive request loop ── */
    for (;;) {
        /* Wait until encrypted TLS record(s) for next request are in buffer */
        bench2_wait_readable(tcp_fd);

        /* ── Read HTTP request headers ── */
        size_t cap = 65536, len = 0;
        char *req = malloc(cap);
        if (!req) break;

        ssize_t hdr_end = -1;
        while (hdr_end < 0) {
            if (len >= 256 * 1024) { free(req); goto done; }   /* too large */
            if (len + 4096 > cap) {
                size_t nc = cap * 2;
                char *nr  = realloc(req, nc);
                if (!nr) { free(req); goto done; }
                req = nr; cap = nc;
            }
            int n = wolfSSL_read(ssl, req + len, (int)(cap - len - 1));
            if (n <= 0) {
                int e = wolfSSL_get_error(ssl, n);
                if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
                    continue;
                free(req); goto done;
            }
            len += (size_t)n;
            hdr_end = find_double_crlf(req, len);
        }

        size_t header_bytes = (size_t)hdr_end + 4;           /* incl. \r\n\r\n */
        long   cl           = parse_content_length(req, (size_t)hdr_end);
        if (cl < 0) cl = 0;
        bool   req_close    = has_connection_close(req, (size_t)hdr_end);

        /* ── Read remaining body if not already in buffer ── */
        size_t body_in_buf = (len > header_bytes) ? len - header_bytes : 0;
        while (body_in_buf < (size_t)cl) {
            size_t want = (size_t)cl - body_in_buf;
            if (header_bytes + (size_t)cl + 1 > cap) {
                size_t nc = header_bytes + (size_t)cl + 1;
                char *nr  = realloc(req, nc);
                if (!nr) { free(req); goto done; }
                req = nr; cap = nc;
            }
            int n = wolfSSL_read(ssl, req + len, (int)want);
            if (n <= 0) {
                int e = wolfSSL_get_error(ssl, n);
                if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
                    continue;
                free(req); goto done;
            }
            len           += (size_t)n;
            body_in_buf   += (size_t)n;
        }

        /* ── Connect to faasd gateway and forward request ── */
        int gw_fd = connect_tcp(gw_host, gw_port);
        if (gw_fd < 0) {
            fprintf(stderr, "[bench2-proxy] connect to %s:%s failed: %s\n",
                    gw_host, gw_port, strerror(errno));
            free(req); goto done;
        }

        if (write_all(gw_fd, req, len) != 0) {
            close(gw_fd); free(req); goto done;
        }
        uint64_t top1 = bench2_rdtsc();
        uint64_t freq = bench2_cntfrq();
        free(req);

        /* ── Read HTTP response from gateway and relay to TLS client ── */
        /* Read response headers */
        size_t  rcap = 65536, rlen = 0;
        char   *resp = malloc(rcap);
        if (!resp) { close(gw_fd); goto done; }

        ssize_t rhdr_end = -1;
        while (rhdr_end < 0) {
            if (rlen + 4096 > rcap) {
                size_t nc = rcap * 2;
                char *nr  = realloc(resp, nc);
                if (!nr) { free(resp); close(gw_fd); goto done; }
                resp = nr; rcap = nc;
            }
            ssize_t n = read(gw_fd, resp + rlen, rcap - rlen - 1);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;   /* EOF or error — forward whatever we have */
            }
            rlen += (size_t)n;
            rhdr_end = find_double_crlf(resp, rlen);
        }

        long resp_cl = -1;
        if (rhdr_end >= 0) {
            resp_cl = parse_content_length(resp, (size_t)rhdr_end);
        }
        size_t resp_hdr_bytes = (rhdr_end >= 0) ? (size_t)rhdr_end + 4 : rlen;
        size_t resp_body_in   = (rlen > resp_hdr_bytes) ? rlen - resp_hdr_bytes : 0;

        /* Read remaining response body */
        if (resp_cl > 0) {
            while (resp_body_in < (size_t)resp_cl) {
                size_t want = (size_t)resp_cl - resp_body_in;
                if (resp_hdr_bytes + (size_t)resp_cl + 1 > rcap) {
                    size_t nc = resp_hdr_bytes + (size_t)resp_cl + 1;
                    char *nr  = realloc(resp, nc);
                    if (!nr) break;
                    resp = nr; rcap = nc;
                }
                ssize_t n = read(gw_fd, resp + rlen, want);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    break;
                }
                rlen         += (size_t)n;
                resp_body_in += (size_t)n;
            }
        } else {
            /* No Content-Length: drain until EOF */
            while (1) {
                if (rlen + 4096 > rcap) {
                    size_t nc = rcap * 2;
                    char *nr  = realloc(resp, nc);
                    if (!nr) break;
                    resp = nr; rcap = nc;
                }
                ssize_t n = read(gw_fd, resp + rlen, rcap - rlen - 1);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    break;
                }
                rlen += (size_t)n;
            }
        }
        close(gw_fd);

        long long top2_ll = -1;
        long long cntfrq_ll = -1;
        long long reqno_ll = -1;
        char worker[128];
        worker[0] = '\0';

        if (resp_hdr_bytes == 0) {
            free(resp);
            goto done;
        }

        top2_ll = parse_header_int64_ci(resp, resp_hdr_bytes, "x-benchfm-top2-rdtsc:");
        cntfrq_ll = parse_header_int64_ci(resp, resp_hdr_bytes, "x-benchfm-cntfrq:");
        reqno_ll = parse_header_int64_ci(resp, resp_hdr_bytes, "x-benchfm-request-no:");
        if (parse_header_string_ci(resp, resp_hdr_bytes, "x-benchfm-worker:", worker, sizeof(worker)) != 0) {
            snprintf(worker, sizeof(worker), "%s", "unknown");
        }

        if (top2_ll <= 0) {
            free(resp);
            goto done;
        }

        uint64_t top2 = (uint64_t)top2_ll;
        uint64_t cntfrq = (cntfrq_ll > 0) ? (uint64_t)cntfrq_ll : freq;
        uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;

        char json[512];
        int jlen = snprintf(json, sizeof(json),
                            "{\"worker\":\"%s\",\"request_no\":%llu"
                            ",\"path\":\"vanilla-forward-tcp\""
                            ",\"top1_rdtsc\":%llu"
                            ",\"top2_rdtsc\":%llu"
                            ",\"delta_cycles\":%llu"
                            ",\"cntfrq\":%llu}\n",
                            worker,
                            (unsigned long long)((reqno_ll > 0) ? (uint64_t)reqno_ll : 0),
                            (unsigned long long)top1,
                            (unsigned long long)top2,
                            (unsigned long long)delta,
                            (unsigned long long)cntfrq);
        free(resp);
        if (jlen <= 0 || (size_t)jlen >= sizeof(json)) {
            goto done;
        }

        char reply[768];
        int rreply = snprintf(reply, sizeof(reply),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: %s\r\n"
                              "\r\n"
                              "%s",
                              jlen,
                              req_close ? "close" : "keep-alive",
                              json);
        if (rreply <= 0 || (size_t)rreply >= sizeof(reply)) {
            goto done;
        }

        if (wolfssl_write_all(ssl, reply, (size_t)rreply) != 0) {
            goto done;
        }

        if (req_close) break;
    }

done:
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_fd);
}

/* ─── main ──────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--listen HOST:PORT] [--upstream HOST:PORT]\n"
            "          [--cert CERT.pem] [--key KEY.pem]\n"
            "Defaults: --listen 0.0.0.0:8444  --upstream 127.0.0.1:8080\n"
            "          --cert /certs/server.crt  --key /certs/server.key\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *listen_str   = "0.0.0.0:8444";
    const char *upstream_str = "127.0.0.1:8080";
    const char *cert_file    = "/certs/server.crt";
    const char *key_file     = "/certs/server.key";

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--listen")   && i+1 < argc) listen_str   = argv[++i];
        else if (!strcmp(argv[i], "--upstream") && i+1 < argc) upstream_str = argv[++i];
        else if (!strcmp(argv[i], "--cert")     && i+1 < argc) cert_file    = argv[++i];
        else if (!strcmp(argv[i], "--key")      && i+1 < argc) key_file     = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    /* Split HOST:PORT */
    char listen_host[256], listen_port[16];
    char gw_host[256],     gw_port[16];

    const char *col = strrchr(listen_str, ':');
    if (!col || col == listen_str) { usage(argv[0]); return 2; }
    snprintf(listen_host, sizeof(listen_host), "%.*s",
             (int)(col - listen_str), listen_str);
    snprintf(listen_port, sizeof(listen_port), "%s", col + 1);

    col = strrchr(upstream_str, ':');
    if (!col || col == upstream_str) { usage(argv[0]); return 2; }
    snprintf(gw_host, sizeof(gw_host), "%.*s",
             (int)(col - upstream_str), upstream_str);
    snprintf(gw_port, sizeof(gw_port), "%s", col + 1);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    wolfSSL_Init();
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx) { fprintf(stderr, "[bench2-proxy] wolfSSL_CTX_new failed\n"); return 1; }

    if (wolfSSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[bench2-proxy] cannot load cert: %s\n", cert_file); return 1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[bench2-proxy] cannot load key: %s\n", key_file); return 1;
    }

    /* Bind and listen */
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_flags    = AI_PASSIVE;
    struct addrinfo *res = NULL;
    if (getaddrinfo(listen_host, listen_port, &hints, &res) != 0) {
        fprintf(stderr, "[bench2-proxy] getaddrinfo failed\n"); return 1;
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
        fprintf(stderr, "[bench2-proxy] bind/listen on %s:%s failed\n",
                listen_host, listen_port);
        return 1;
    }

    fprintf(stderr, "[bench2-proxy] listening on %s:%s → upstream %s:%s\n",
            listen_host, listen_port, gw_host, gw_port);

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("[bench2-proxy] accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("[bench2-proxy] fork");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            /* child */
            close(listen_fd);
            handle_connection(client_fd, ctx, gw_host, gw_port);
            wolfSSL_CTX_free(ctx);
            wolfSSL_Cleanup();
            exit(0);
        }
        /* parent */
        close(client_fd);
    }
}
