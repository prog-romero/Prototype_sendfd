/*
 * tls_listener.h — C bridge for wolfSSL TLS operations in the gateway.
 *
 * Architecture (epoll-driven, single event loop — mirrors bench2gw/server.go):
 *
 *   wolfssl_do_handshake_start()   : allocate conn, set non-blocking, begin wolfSSL_accept
 *   wolfssl_do_handshake_step()    : drive one step of the non-blocking handshake
 *   wolfssl_conn_want_events()     : 1=EPOLLIN, 2=EPOLLOUT (what the conn needs next)
 *   tlsgw_peek_and_export_nb()     : non-blocking peek + export (returns 0=EAGAIN, >0=rawfd, -1=not-fn, -2=err)
 *
 * The Go side runs ONE shared epoll loop that drives ALL connections:
 *   - listen fd       → accept new TCP connections
 *   - per-client fd   → drive handshake state machine, then peek+export
 *   - relay listen fd → accept relay FDs from workers
 *   - relay conn fd   → receive SCM_RIGHTS from wrong-owner workers
 *
 * This eliminates one goroutine (and one OS thread) per connection.
 * When idle, all work is blocked in a single EpollWait().
 * CPU immediately drops to 0 when no clients are connected.
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

/* Opaque per-connection handle (holds tcp_fd + WOLFSSL* + handshake state). */
typedef struct wolfssl_gtw_conn wolfssl_gtw_conn_t;

/* Connection states */
typedef enum {
    WOLFSSL_GTW_CONN_HANDSHAKE = 0,
    WOLFSSL_GTW_CONN_PEEK,
    WOLFSSL_GTW_CONN_DONE,
} wolfssl_gtw_conn_state_t;

/*
 * wolfssl_gtw_ctx_new() — Initialise wolfSSL and load certificate + private key.
 */
wolfssl_gtw_ctx_t *wolfssl_gtw_ctx_new(const char *cert_file, const char *key_file);

/*
 * wolfssl_gtw_ctx_free() — Release the wolfSSL CTX.
 */
void wolfssl_gtw_ctx_free(wolfssl_gtw_ctx_t *ctx);

/*
 * wolfssl_accept_start() — Accept from listen_fd and create a non-blocking conn.
 * Returns NULL + sets errno=EAGAIN if no connection is pending.
 * On success, the returned conn is in HANDSHAKE state.
 */
wolfssl_gtw_conn_t *wolfssl_accept_start(wolfssl_gtw_ctx_t *ctx, int listen_fd);

/*
 * wolfssl_conn_fd() — The TCP fd of the connection.
 */
int wolfssl_conn_fd(const wolfssl_gtw_conn_t *conn);

/*
 * wolfssl_conn_want_events() — What epoll events this conn needs next.
 * Returns 1=EPOLLIN, 2=EPOLLOUT.
 */
int wolfssl_conn_want_events(const wolfssl_gtw_conn_t *conn);

/*
 * wolfssl_conn_state() — Current state of the connection.
 */
wolfssl_gtw_conn_state_t wolfssl_conn_state(const wolfssl_gtw_conn_t *conn);

/*
 * wolfssl_handshake_step() — Drive one step of the non-blocking TLS handshake.
 *
 * Returns:
 *   0  : handshake in progress (WANT_READ/WANT_WRITE) — re-arm epoll with wolfssl_conn_want_events()
 *   1  : handshake complete, conn transitions to PEEK state
 *  -1  : fatal error — call wolfssl_conn_free()
 */
int wolfssl_handshake_step(wolfssl_gtw_conn_t *conn);

/*
 * tlsgw_peek_and_export_nb() — Non-blocking peek and TLS session export.
 *
 * MUST only be called when conn is in PEEK state (after handshake_step returns 1).
 *
 * Returns:
 *   raw_fd > 0 : /function/<name> path — wolfSSL detached, raw_fd ready for sendfd.
 *                fn_name_out is populated. serial_buf/serial_sz_out are populated.
 *              0 : data not yet available (EAGAIN) — re-arm epoll EPOLLIN for this fd.
 *             -1 : not a /function/ path — wolfSSL still active, use for ChanListener.
 *             -2 : fatal error — conn has been freed internally, do NOT call wolfssl_conn_free().
 */
int tlsgw_peek_and_export_nb(
    wolfssl_gtw_conn_t *conn,
    char               *fn_name_out,
    size_t              fn_name_sz,
    uint64_t           *top1_ns_out,
    void               *serial_buf,
    int                *serial_sz_out,
    int                 skip_top1
);

/*
 * wolfssl_conn_free() — Free a connection and close its fd.
 * Safe to call in any state.
 */
void wolfssl_conn_free(wolfssl_gtw_conn_t *conn);

/*
 * wolfssl_conn_take_fd() — Detach and return the TCP fd from a conn.
 * The conn no longer owns the fd after this call.
 * Used when pushing to ChanListener (conn still owns wolfSSL, not the raw fd).
 */
int wolfssl_conn_take_raw_fd(wolfssl_gtw_conn_t *conn);

/*
 * wolfssl_accept_start_plain() — Like wolfssl_accept_start() but does NOT install
 * the tlspeek keylog callback.  Use for the vanilla HTTPS path (RunVanillaHTTPS)
 * where libtlspeek is never needed: handshake completes, conn is pushed to
 * ChanListener, and all reads/writes go through wolfSSL_read/wolfSSL_write only.
 */
wolfssl_gtw_conn_t *wolfssl_accept_start_plain(wolfssl_gtw_ctx_t *ctx, int listen_fd);

/* ── Vanilla wolfSSL read/write for ChanListener path ── */

int  wolfssl_gtw_conn_read(wolfssl_gtw_conn_t *conn, void *buf, int len);
int  wolfssl_gtw_conn_write(wolfssl_gtw_conn_t *conn, const void *buf, int len);
int  wolfssl_gtw_conn_get_error(wolfssl_gtw_conn_t *conn, int ret);
int  wolfssl_gtw_conn_fd(const wolfssl_gtw_conn_t *conn);
int  wolfssl_gtw_conn_pending(const wolfssl_gtw_conn_t *conn);
void wolfssl_gtw_conn_close(wolfssl_gtw_conn_t *conn);

/*
 * TLSGW_SERIAL_SIZE — exact byte size of a serialised TLS session state.
 */
extern const int TLSGW_SERIAL_SIZE;

#ifdef __cplusplus
}
#endif

#endif /* TLS_LISTENER_H */
