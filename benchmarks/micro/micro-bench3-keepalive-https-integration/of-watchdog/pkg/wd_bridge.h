/*
 * wd_bridge.h — CGO bridge: the watchdog's full-proxy connection driver.
 *
 * This bridge contains the connection plumbing that used to live inside the C
 * function workers (timing_fn_ka_worker.c for HTTP and
 * timing_fn_ka_worker_https.c for HTTPS):
 *
 *   - HTTP  : raw recv/send, owner peek, relay, keep-alive, request framing.
 *   - HTTPS : wolfSSL session restore from the gateway-exported TLS state,
 *             tls_read_peek for owner detection, wolfSSL_read/write, TLS state
 *             re-export across keep-alive and relay.
 *
 * The only thing the bridge does NOT do is the business logic: for every fully
 * assembled plaintext HTTP request it calls back into Go (goInvokeHandler),
 * which runs the watchdog's normal requestHandler (reverse-proxy to the plain
 * HTTP function) and returns the response bytes to send to the client.
 *
 * One bridge call drives one migrated connection, blocking, for its whole
 * keep-alive lifetime. The watchdog runs one goroutine per connection.
 */
#ifndef WD_BRIDGE_H
#define WD_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

/*
 * wd_bridge_init — create the wolfSSL server context once at start-up.
 *
 * cert_file / key_file are the server certificate and private key used to
 * restore TLS sessions. If they are NULL or empty, HTTPS connections are
 * rejected (HTTP still works).
 *
 * Returns 0 on success, -1 on error.
 */
int wd_bridge_init(const char *cert_file, const char *key_file);

/*
 * wd_serve_conn — drive one migrated connection to completion.
 *
 * Takes ownership of client_fd and pipe_fd (both are closed before return).
 *
 *   payload      raw bytes received from the gateway: a base header, optionally
 *                followed by the serialised TLS state (HTTPS).
 *   payload_len  number of valid bytes in payload.
 *   own_fn_name  this function's name, used to decide handle-vs-relay.
 *   relay_sock   provider socket path used to relay wrong-owner connections.
 *   gohandle     opaque cgo.Handle passed back to goInvokeHandler.
 */
void wd_serve_conn(int client_fd, int pipe_fd,
                   const unsigned char *payload, int payload_len,
                   const char *own_fn_name, const char *relay_sock,
                   uintptr_t gohandle);

/*
 * goInvokeHandler — implemented in Go (see fullproxy_cgo.go).
 *
 * Runs one plaintext HTTP request through the watchdog requestHandler and
 * returns the response wire bytes.
 *
 *   req / req_len    full plaintext request (request-line + headers + body).
 *   should_close     1 if the client requested Connection: close.
 *   resp_out         set to a malloc'd buffer with the response wire bytes;
 *                    the bridge frees it after sending.
 *   resp_len_out     length of *resp_out.
 *
 * Returns 0 on success, non-zero on failure.
 *
 * NOTE: the pointer parameters are intentionally non-const so this prototype
 * matches exactly the signature cgo generates for the //export directive.
 */
extern int goInvokeHandler(uintptr_t gohandle,
                           unsigned char *req, int req_len,
                           int should_close,
                           unsigned char **resp_out, int *resp_len_out);

#endif /* WD_BRIDGE_H */
