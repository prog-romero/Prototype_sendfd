/*
 * tls_vanilla.c — Vanilla HTTPS gateway: pure wolfSSL, no libtlspeek.
 *
 * The ONLY wolfSSL operations here are:
 *   - wolfSSL_CTX_new / wolfSSL_new : context + session creation
 *   - wolfSSL_accept                : TLS 1.3 server handshake
 *   - wolfSSL_read / wolfSSL_write  : decrypt incoming / encrypt outgoing data
 *   - wolfSSL_free / wolfSSL_CTX_free : cleanup
 *
 * No keylog callback. No secret callback. No libtlspeek dependency.
 */

/* wolfSSL public API — no special defines needed for vanilla mode */
#include "tls_vanilla.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <stdlib.h>
#include <unistd.h>

/* ── Internal structs ──────────────────────────────────────────────────── */

struct wolfssl_vanilla_ctx {
    WOLFSSL_CTX *wctx;
};

struct wolfssl_vanilla_conn {
    int      tcp_fd;
    WOLFSSL *ssl;
};

/* ── Context ───────────────────────────────────────────────────────────── */

wolfssl_vanilla_ctx_t *wolfssl_vanilla_ctx_new(const char *cert_file,
                                               const char *key_file)
{
    if (!cert_file || !key_file) return NULL;

    wolfSSL_Init();

    wolfssl_vanilla_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx->wctx) { free(ctx); return NULL; }

    /* Restrict to TLS 1.3 only (same as prototype path) */
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

void wolfssl_vanilla_ctx_free(wolfssl_vanilla_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->wctx) wolfSSL_CTX_free(ctx->wctx);
    free(ctx);
    wolfSSL_Cleanup();
}

/* ── Handshake ─────────────────────────────────────────────────────────── */

wolfssl_vanilla_conn_t *wolfssl_vanilla_handshake(wolfssl_vanilla_ctx_t *ctx,
                                                   int tcp_fd)
{
    if (!ctx || !ctx->wctx || tcp_fd < 0) return NULL;

    wolfssl_vanilla_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { close(tcp_fd); return NULL; }

    conn->tcp_fd = tcp_fd;
    conn->ssl    = wolfSSL_new(ctx->wctx);
    if (!conn->ssl) { free(conn); close(tcp_fd); return NULL; }

    wolfSSL_set_fd(conn->ssl, tcp_fd);

    /* Blocking TLS 1.3 handshake — no callbacks, no key capture */
    if (wolfSSL_accept(conn->ssl) != SSL_SUCCESS) {
        wolfSSL_free(conn->ssl);
        free(conn);
        close(tcp_fd);
        return NULL;
    }
    return conn;
}

/* ── Read / Write / Error / Close ──────────────────────────────────────── */

int wolfssl_vanilla_read(wolfssl_vanilla_conn_t *conn, void *buf, int len)
{
    if (!conn || !conn->ssl || !buf || len <= 0) return -1;
    for (;;) {
        int n = wolfSSL_read(conn->ssl, buf, len);
        if (n > 0) return n;
        int e = wolfSSL_get_error(conn->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        return n; /* 0 = clean close, <0 = error */
    }
}

int wolfssl_vanilla_write(wolfssl_vanilla_conn_t *conn, const void *buf, int len)
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

int wolfssl_vanilla_get_error(wolfssl_vanilla_conn_t *conn, int ret)
{
    if (!conn || !conn->ssl) return -1;
    return wolfSSL_get_error(conn->ssl, ret);
}

void wolfssl_vanilla_close(wolfssl_vanilla_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
    }
    if (conn->tcp_fd >= 0) close(conn->tcp_fd);
    free(conn);
}
