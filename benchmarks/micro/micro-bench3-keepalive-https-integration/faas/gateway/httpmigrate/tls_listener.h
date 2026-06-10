/*
 * tls_listener.h — C bridge for wolfSSL TLS operations in the gateway.
 *
 * Used for BOTH vanilla and prototype HTTPS modes.
 *
 * Design:
 *   - Go accept loop calls syscall.Accept() (fast — just OS queue dequeue).
 *   - Each accepted fd is passed to a goroutine (lightweight Go thread).
 *   - The goroutine calls wolfssl_do_handshake() to perform the TLS negotiation.
 *   - This gives true parallel (concurrent) handshakes: one goroutine per connection.
 *
 * For VANILLA mode:
 *   wolfssl_do_handshake() → wolfssl_conn_t* → Go wraps it as net.Conn
 *   → pushed to ChanListener → OpenFaaS http.Server handles it normally.
 *
 * For PROTOTYPE mode:
 *   wolfssl_do_handshake() → wolfssl_conn_t*
 *   → tlsgw_peek_and_export() inspects (decrypts without consuming) the request
 *   → if /function/*: export TLS state, detach wolfSSL, return raw tcp_fd for sendfd
 *   → else: same path as vanilla (push to ChanListener)
 */
#ifndef TLS_LISTENER_H
#define TLS_LISTENER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Opaque context (holds WOLFSSL_CTX* shared across all connections). */
typedef struct wolfssl_gtw_ctx  wolfssl_gtw_ctx_t;

/* Opaque per-connection handle (holds tcp_fd + WOLFSSL*). */
typedef struct wolfssl_gtw_conn wolfssl_gtw_conn_t;

/*
 * wolfssl_gtw_ctx_new() — Initialise wolfSSL and load certificate + private key.
 *
 * cert_file : path to PEM certificate (e.g. "/certs/server.crt")
 * key_file  : path to PEM private key  (e.g. "/certs/server.key")
 *
 * Returns a ctx pointer on success, NULL on failure.
 */
wolfssl_gtw_ctx_t *wolfssl_gtw_ctx_new(const char *cert_file, const char *key_file);

/*
 * wolfssl_gtw_ctx_free() — Release the wolfSSL CTX.
 */
void wolfssl_gtw_ctx_free(wolfssl_gtw_ctx_t *ctx);

/*
 * wolfssl_do_handshake() — Perform TLS 1.3 handshake on an already-accepted fd.
 *
 * Called from a Go goroutine (one per connection) so handshakes run in parallel.
 *
 * tcp_fd : raw file descriptor returned by syscall.Accept() from Go.
 *
 * Returns a conn handle on success, NULL on failure (tcp_fd is closed on failure).
 * The tlspeek_keylog_cb is installed automatically so traffic keys are captured.
 */
wolfssl_gtw_conn_t *wolfssl_do_handshake(wolfssl_gtw_ctx_t *ctx, int tcp_fd);

/* ── Read / Write / Close (used by vanilla net.Conn wrapper) ── */

int  wolfssl_gtw_conn_read(wolfssl_gtw_conn_t *conn, void *buf, int len);
int  wolfssl_gtw_conn_write(wolfssl_gtw_conn_t *conn, const void *buf, int len);
int  wolfssl_gtw_conn_get_error(wolfssl_gtw_conn_t *conn, int ret);
int  wolfssl_gtw_conn_fd(const wolfssl_gtw_conn_t *conn);
int  wolfssl_gtw_conn_pending(const wolfssl_gtw_conn_t *conn);
void wolfssl_gtw_conn_close(wolfssl_gtw_conn_t *conn);

/* ── Prototype-specific: peek + export ── */

/*
 * tlsgw_peek_and_export() — Used in prototype (HTTPMIGRATE_ENABLE=1) mode only.
 *
 * Steps:
 *   1. epoll-wait on conn's tcp_fd until encrypted bytes are in kernel buffer.
 *   2. If skip_top1 == 0: stamp top1 = CLOCK_MONOTONIC_RAW nanoseconds.
 *   3. tls_read_peek(): stateless AEAD decrypt via MSG_PEEK (kernel buffer unchanged).
 *   4. Parse "/function/<name>" from the plaintext first line.
 *   5. If a function name is found:
 *        - wolfSSL_tls_export() to save full session state into serial_buf.
 *        - Detach wolfSSL from tcp_fd (wolfSSL_set_fd(ssl, -1) + wolfSSL_free).
 *        - Return the raw tcp_fd (ready for sendfd).
 *      Else (not a /function/ path):
 *        - Leave wolfSSL session active on conn (caller pushes to ChanListener).
 *        - Return -1 with fn_name_out[0] == '\0'.
 *
 * Parameters:
 *   conn          : connection handle from wolfssl_do_handshake().
 *   fn_name_out   : output buffer for the function name (e.g. "timing-fn-a").
 *   fn_name_sz    : size of fn_name_out buffer (128 recommended).
 *   top1_ns_out   : output: top1 timestamp in nanoseconds (0 if skip_top1==1).
 *   serial_buf    : output: raw bytes of tlspeek_serial_t (must be TLSGW_SERIAL_SIZE bytes).
 *   serial_sz_out : output: actual bytes written into serial_buf.
 *   skip_top1     : if 1, set *top1_ns_out = 0 (SUM_PROD mode — no timing).
 *
 * Returns: raw tcp_fd (>= 0) if it is a /function/ path and sendfd should happen.
 *          -1 if it is NOT a /function/ path (wolfSSL conn still active for ChanListener).
 *          -2 on fatal error (conn is freed internally).
 */
int tlsgw_peek_and_export(
    wolfssl_gtw_conn_t *conn,
    char               *fn_name_out,
    size_t              fn_name_sz,
    uint64_t           *top1_ns_out,
    void               *serial_buf,
    int                *serial_sz_out,
    int                 skip_top1
);

/*
 * TLSGW_SERIAL_SIZE — exact byte size of a serialised (packed) TLS session state.
 * Must match sizeof(tlspeek_serial_t) from libtlspeek.
 * Used by Go to allocate the serial buffer passed to tlsgw_peek_and_export().
 */
extern const int TLSGW_SERIAL_SIZE;

#ifdef __cplusplus
}
#endif

#endif /* TLS_LISTENER_H */
