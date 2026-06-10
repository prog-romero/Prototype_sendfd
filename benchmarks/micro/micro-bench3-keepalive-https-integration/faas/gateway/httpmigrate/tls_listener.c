/*
 * tls_listener.c — wolfSSL gateway bridge implementation.
 *
 * Provides TLS 1.3 handshake, read/write/close helpers (for vanilla net.Conn),
 * and tls_read_peek-based function routing (for prototype sendfd path).
 *
 * Build requirements:
 *   CGO_ENABLED=1
 *   -I<wolfssl-include>  -I<libtlspeek-include>
 *   -L<wolfssl-lib>      -lwolfssl
 *   -L<libtlspeek-lib>   -ltlspeek
 */

/* Required before any wolfSSL header to unlock secret-callback support */
#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include "tls_listener.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* libtlspeek: stateless TLS 1.3 peek + session serialisation */
#include <tlspeek/tlspeek.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

/* ── TLSGW_SERIAL_SIZE ─────────────────────────────────────────────────── */

/* Exported constant so Go can allocate the right buffer size without CGo
 * sizeof magic (which is cumbersome — difficult/awkward — with structs). */
const int TLSGW_SERIAL_SIZE = (int)sizeof(tlspeek_serial_t);

/* ── Internal structs ─────────────────────────────────────────────────── */

struct wolfssl_gtw_ctx {
    WOLFSSL_CTX *wctx;
};

struct wolfssl_gtw_conn {
    int      tcp_fd;
    WOLFSSL *ssl;
    /* tlspeek context — populated during handshake via keylog callback */
    tlspeek_ctx_t peek_ctx;
};

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Timestamp in nanoseconds using CLOCK_MONOTONIC_RAW (hardware clock —
 * hardware timer not affected by NTP adjustments). */
static uint64_t now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Block (wait without busy-spinning) until tcp_fd has incoming data.
 * Uses epoll (Linux event notification — mechanism to watch multiple fds
 * efficiently without looping). */
static int wait_readable(int tcp_fd)
{
    int epfd = epoll_create1(0);
    if (epfd < 0) return -1;

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    ev.data.fd = tcp_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tcp_fd, &ev) != 0) {
        close(epfd);
        return -1;
    }

    struct epoll_event out;
    int result = -1;
    for (;;) {
        int n = epoll_wait(epfd, &out, 1, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue; /* spurious wakeup — false alarm */
        result = (out.events & EPOLLIN) ? 0 : -1;
        break;
    }
    close(epfd);
    return result;
}

/* Parse "/function/<name>" from the first line of a plaintext HTTP request.
 * Returns 1 if found and name written to out, 0 otherwise. */
static int parse_fn_name(const unsigned char *buf, size_t len,
                         char *out, size_t out_sz)
{
    if (!buf || !len || !out || !out_sz) return 0;

    size_t eol = 0;
    while (eol < len && buf[eol] != '\n') eol++;

    const unsigned char *sp1 = memchr(buf, ' ', eol);
    if (!sp1) return 0;

    const unsigned char *path = sp1 + 1;
    static const char prefix[] = "/function/";
    size_t plen = eol - (size_t)(path - buf);
    const unsigned char *sp2 = memchr(path, ' ', plen);
    if (!sp2) return 0;

    size_t rlen = (size_t)(sp2 - path);
    if (rlen <= sizeof(prefix) - 1 ||
        memcmp(path, prefix, sizeof(prefix) - 1) != 0)
        return 0;

    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++) {
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            nlen = i;
            break;
        }
    }
    if (!nlen || nlen + 1 > out_sz) return 0;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return 1;
}

/* Set cipher suite in tlspeek_serial_t from wolfSSL cipher string. */
static void set_serial_cipher(tlspeek_serial_t *s, const char *name)
{
    if (!s) return;
    if (!name)           { s->cipher_suite = TLSPEEK_AES_256_GCM; return; }
    if (strstr(name, "CHACHA20")) s->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES-128")) s->cipher_suite = TLSPEEK_AES_128_GCM;
    else                              s->cipher_suite = TLSPEEK_AES_256_GCM;
}

/* ── Public API ───────────────────────────────────────────────────────── */

wolfssl_gtw_ctx_t *wolfssl_gtw_ctx_new(const char *cert_file,
                                        const char *key_file)
{
    if (!cert_file || !key_file) return NULL;

    wolfSSL_Init();

    wolfssl_gtw_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx->wctx) { free(ctx); return NULL; }

    /* Restrict to TLS 1.3 only */
    wolfSSL_CTX_SetMinVersion(ctx->wctx, WOLFSSL_TLSV1_3);
    wolfSSL_CTX_set_max_proto_version(ctx->wctx, WOLFSSL_TLSV1_3);

    if (wolfSSL_CTX_use_certificate_file(ctx->wctx, cert_file,
                                          SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(ctx->wctx, key_file,
                                         SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx->wctx);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void wolfssl_gtw_ctx_free(wolfssl_gtw_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->wctx) wolfSSL_CTX_free(ctx->wctx);
    free(ctx);
    wolfSSL_Cleanup();
}

/* ── wolfssl_do_handshake ─────────────────────────────────────────────
 * Called from a Go goroutine — one goroutine per accepted TCP connection.
 * Parallel (concurrent) handshakes: N goroutines = N simultaneous TLS setups.
 */
wolfssl_gtw_conn_t *wolfssl_do_handshake(wolfssl_gtw_ctx_t *ctx, int tcp_fd)
{
    if (!ctx || !ctx->wctx || tcp_fd < 0) return NULL;

    wolfssl_gtw_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { close(tcp_fd); return NULL; }

    conn->tcp_fd = tcp_fd;

    conn->ssl = wolfSSL_new(ctx->wctx);
    if (!conn->ssl) { free(conn); close(tcp_fd); return NULL; }

    wolfSSL_set_fd(conn->ssl, tcp_fd);

    /* Prepare peek context for the keylog callback */
    memset(&conn->peek_ctx, 0, sizeof(conn->peek_ctx));
    conn->peek_ctx.tcp_fd    = tcp_fd;
    conn->peek_ctx.ssl       = conn->ssl;
    conn->peek_ctx.keys_ready = 1; /* Default: allow vanilla HTTPS (keys only
                                      needed for prototype sendfd peek path) */

    /* Install secret callback — fires during handshake, captures traffic keys */
    wolfSSL_set_ex_data(conn->ssl, 0, &conn->peek_ctx);
    wolfSSL_set_tls13_secret_cb(conn->ssl, tlspeek_keylog_cb, &conn->peek_ctx);

    /* Perform TLS 1.3 handshake (blocking — waits for client) */
    if (wolfSSL_accept(conn->ssl) != SSL_SUCCESS) {
        wolfSSL_free(conn->ssl);
        free(conn);
        close(tcp_fd);
        return NULL;
    }

    if (!conn->peek_ctx.keys_ready) {
        /* Keylog callback did not fire — keys not captured, abort */
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
        free(conn);
        close(tcp_fd);
        return NULL;
    }

    /* Set cipher suite in peek context from negotiated cipher name */
    const char *cname = wolfSSL_get_cipher_name(conn->ssl);
    if (cname) {
        if (strstr(cname, "CHACHA20"))
            conn->peek_ctx.cipher_suite = TLSPEEK_CHACHA20_POLY;
        else if (strstr(cname, "AES-128"))
            conn->peek_ctx.cipher_suite = TLSPEEK_AES_128_GCM;
        else
            conn->peek_ctx.cipher_suite = TLSPEEK_AES_256_GCM;
    }

    return conn;
}

/* ── Read / Write / Close — vanilla net.Conn helpers ─────────────────── */

int wolfssl_gtw_conn_read(wolfssl_gtw_conn_t *conn, void *buf, int len)
{
    if (!conn || !conn->ssl || !buf || len <= 0) return -1;
    return wolfSSL_read(conn->ssl, buf, len);
}

int wolfssl_gtw_conn_write(wolfssl_gtw_conn_t *conn, const void *buf, int len)
{
    if (!conn || !conn->ssl || !buf || len <= 0) return -1;
    
    int n = wolfSSL_write(conn->ssl, buf, len);
    if (n <= 0) {
        int e = wolfSSL_get_error(conn->ssl, n);
        if (e == SSL_ERROR_WANT_READ) {
            char drain[1024];
            wolfSSL_read(conn->ssl, drain, (int)sizeof(drain));
        }
    }
    return n;
}

int wolfssl_gtw_conn_get_error(wolfssl_gtw_conn_t *conn, int ret)
{
    if (!conn || !conn->ssl) return -1;
    return wolfSSL_get_error(conn->ssl, ret);
}

int wolfssl_gtw_conn_fd(const wolfssl_gtw_conn_t *conn)
{
    return conn ? conn->tcp_fd : -1;
}

int wolfssl_gtw_conn_pending(const wolfssl_gtw_conn_t *conn)
{
    if (!conn || !conn->ssl) return 0;
    return wolfSSL_pending(conn->ssl);
}

void wolfssl_gtw_conn_close(wolfssl_gtw_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
    }
    if (conn->tcp_fd >= 0) close(conn->tcp_fd);
    tlspeek_free(&conn->peek_ctx);
    free(conn);
}

/* ── tlsgw_peek_and_export — prototype sendfd routing ────────────────── */

int tlsgw_peek_and_export(
    wolfssl_gtw_conn_t *conn,
    char               *fn_name_out,
    size_t              fn_name_sz,
    uint64_t           *top1_ns_out,
    void               *serial_buf,
    int                *serial_sz_out,
    int                 skip_top1)
{
    if (!conn || !fn_name_out || fn_name_sz == 0) return -2;

    fn_name_out[0] = '\0';
    if (top1_ns_out)   *top1_ns_out   = 0;
    if (serial_sz_out) *serial_sz_out = 0;

    /* Wait in Go before calling this */
    /* 2. Stamp top1 (unless SUM_PROD mode) */
    if (!skip_top1 && top1_ns_out)
        *top1_ns_out = now_ns();

    /* Wait in Go before calling this */
    uint8_t peek_buf[4096];
    int peeked = tls_read_peek(&conn->peek_ctx, peek_buf, sizeof(peek_buf) - 1);
    if (peeked == 0) {
        return 0; /* Need more data */
    }
    if (peeked < 0) {
        /* peek failed — treat as non-function, leave conn for ChanListener */
        return -1;
    }
    peek_buf[peeked] = '\0';

    /* 4. Parse function name from plaintext request line */
    if (!parse_fn_name(peek_buf, (size_t)peeked, fn_name_out, fn_name_sz)) {
        /* Not a /function/ path — leave wolfSSL active for ChanListener */
        return -1;
    }

    /* 5. Function found — export full TLS session state */
    tlspeek_serial_t *serial = (tlspeek_serial_t *)serial_buf;
    if (!serial) {
        wolfssl_gtw_conn_close(conn);
        return -2;
    }

    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    set_serial_cipher(serial, wolfSSL_get_cipher_name(conn->ssl));

    /* Export wolfSSL full session blob (used by worker for wolfSSL_tls_import) */
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    int rc = wolfSSL_tls_export(conn->ssl, serial->tls_blob, &blob_sz);
    if (rc <= 0) {
        fprintf(stderr, "[tls_gw] wolfSSL_tls_export failed: %d\n", rc);
        wolfssl_gtw_conn_close(conn);
        return -2;
    }
    serial->blob_sz = blob_sz;

    /* Copy client write key + IV (needed by worker for tls_read_peek on KA) */
    memcpy(serial->client_write_key,
           conn->peek_ctx.client_write_key,
           sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv,
           conn->peek_ctx.client_write_iv,
           sizeof(serial->client_write_iv));
    serial->read_seq_num = conn->peek_ctx.read_seq_num;

    if (serial_sz_out) *serial_sz_out = (int)sizeof(tlspeek_serial_t);

    /* 6. Detach wolfSSL from tcp_fd without sending TLS close_notify alert */
    int raw_fd = conn->tcp_fd;
    wolfSSL_set_quiet_shutdown(conn->ssl, 1);
    wolfSSL_set_fd(conn->ssl, -1); /* detach — disconnect wolfSSL from the socket */
    wolfSSL_free(conn->ssl);
    conn->ssl    = NULL;
    conn->tcp_fd = -1;
    tlspeek_free(&conn->peek_ctx);
    free(conn);

    /* 7. Return raw tcp_fd — ready for sendfd to of-watchdog */
    return raw_fd;
}
