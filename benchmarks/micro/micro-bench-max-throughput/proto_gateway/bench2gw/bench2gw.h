/*
 * bench2gw.h — C shim API for the bench2 prototype gateway.
 *
 * Provides the functions called from Go via CGO:
 *
 *   bench2gw_ctx_t *bench2gw_new_ctx(cert, key)
 *   void            bench2gw_free_ctx(ctx)
 *
 *   int  bench2gw_accept_peek_export(ctx, listen_fd,
 *                                    peek_buf, peek_buf_sz, peek_len_out,
 *                                    payload_out)
 *        Accepts one TCP connection, performs the TLS handshake, then:
 *          bench2_wait_readable(client_fd)    ← FIONREAD spin
 *          top1 = bench2_rdtsc()              ← stamp
 *          tls_read_peek(...)                 ← peek without consuming
 *        Fills payload_out->top1_rdtsc / cntfrq / top1_set and the serial.
 *        Returns the raw TCP fd (>= 0) or -1 on error.
 *
 *   int  bench2gw_send_fd(sock_path, client_fd, payload, payload_len)
 *        Connects to the worker's Unix socket and transfers client_fd plus
 *        the payload via sendfd_with_state().  Closes client_fd on success.
 *
 *   int  bench2gw_unix_server(sock_path)
 *        Creates a listening Unix-domain socket for the relay.
 *
 *   int  bench2gw_accept_recv(listen_fd, payload_out)
 *        Accepts one connection on the relay socket, receives the fd and
 *        payload (with top1_rdtsc already set by the wrong-owner container).
 *        Returns the received TCP fd or -1.
 */
#ifndef BENCH2GW_H
#define BENCH2GW_H

#ifndef HAVE_SECRET_CALLBACK
#  define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#  define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <tlspeek/tlspeek.h>
#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

#include <stdint.h>
#include <stddef.h>

#include "bench2_rdtsc.h"   /* bench2_rdtsc, bench2_cntfrq, bench2_wait_readable */
#include "bench2_payload.h" /* bench2_keepalive_payload_t */

/* Opaque context holding the wolfSSL_CTX for the gateway listener. */
typedef struct bench2gw_ctx bench2gw_ctx_t;

bench2gw_ctx_t *bench2gw_new_ctx(const char *cert_file, const char *key_file);
void            bench2gw_free_ctx(bench2gw_ctx_t *ctx);

int bench2gw_accept_peek_export(
    bench2gw_ctx_t               *ctx,
    int                           listen_fd,
    unsigned char                *peek_buf,
    size_t                        peek_buf_sz,
    int                          *peek_len_out,
    bench2_keepalive_payload_t   *payload_out);

int bench2gw_send_fd(
    const char                         *unix_sock_path,
    int                                 client_fd,
    const bench2_keepalive_payload_t   *payload,
    size_t                              payload_len);

int bench2gw_unix_server(const char *sock_path);

int bench2gw_accept_recv(int listen_fd, bench2_keepalive_payload_t *payload_out);

#endif /* BENCH2GW_H */
