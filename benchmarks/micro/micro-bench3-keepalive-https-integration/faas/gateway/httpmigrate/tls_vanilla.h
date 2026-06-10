/*
 * tls_vanilla.h — Vanilla HTTPS gateway: pure wolfSSL, no libtlspeek.
 *
 * Used ONLY by RunVanillaHTTPS. The prototype (sendfd) path uses tls_listener.h.
 * There is no keylog callback, no peek context, no TLS session export here.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct wolfssl_vanilla_ctx  wolfssl_vanilla_ctx_t;
typedef struct wolfssl_vanilla_conn wolfssl_vanilla_conn_t;

/*
 * wolfssl_vanilla_ctx_new — Create a TLS 1.3 server context.
 * cert_file / key_file : PEM-encoded certificate and private key paths.
 * Returns NULL on failure.
 */
wolfssl_vanilla_ctx_t *wolfssl_vanilla_ctx_new(const char *cert_file,
                                               const char *key_file);

/* Free the context (calls wolfSSL_CTX_free + wolfSSL_Cleanup). */
void wolfssl_vanilla_ctx_free(wolfssl_vanilla_ctx_t *ctx);

/*
 * wolfssl_vanilla_handshake — Perform TLS 1.3 server handshake on tcp_fd.
 * Blocking. On failure closes tcp_fd and returns NULL.
 */
wolfssl_vanilla_conn_t *wolfssl_vanilla_handshake(wolfssl_vanilla_ctx_t *ctx,
                                                   int tcp_fd);

/* wolfssl_vanilla_read  — wraps wolfSSL_read  (blocking, retries WANT_*). */
int wolfssl_vanilla_read(wolfssl_vanilla_conn_t *conn, void *buf, int len);

/* wolfssl_vanilla_write — wraps wolfSSL_write (blocking, retries WANT_*). */
int wolfssl_vanilla_write(wolfssl_vanilla_conn_t *conn, const void *buf, int len);

/* wolfssl_vanilla_get_error — wraps wolfSSL_get_error. */
int wolfssl_vanilla_get_error(wolfssl_vanilla_conn_t *conn, int ret);

/* wolfssl_vanilla_close — quiet shutdown, free ssl object, close tcp_fd. */
void wolfssl_vanilla_close(wolfssl_vanilla_conn_t *conn);

#ifdef __cplusplus
}
#endif
