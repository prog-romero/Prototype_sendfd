/*
 * worker.c — Worker process: receives the TLS connection fd from the gateway
 * via SCM_RIGHTS, restores the TLS session state, reads the HTTP request
 * (the SAME data that tls_read_peek() saw, because the kernel buffer was
 * never consumed), and sends the HTTP response directly to the client.
 *
 * Usage:  ./worker <worker_id>
 * Example: ./worker 0
 *          ./worker 1
 *
 * The worker must start BEFORE the gateway so its Unix socket exists when
 * the gateway tries to connect.
 */

/* Ensure wolfSSL features are enabled */
#ifndef WOLFSSL_TLS13
#define WOLFSSL_TLS13
#endif
#ifndef HAVE_HKDF
#define HAVE_HKDF
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "tlspeek.h"
#include "tlspeek_serial.h"
#include "sendfd.h"
#include "unix_socket.h"
#include "handler.h"

#define UNIX_BACKLOG  8
#define REQUEST_BUFSZ 65536

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * handle_incoming_fd() — Called for each fd received from the gateway.
 *
 * conn_fd    : the already-accepted Unix socket connection from the gateway
 * wctx       : the worker's wolfSSL_CTX (pre-loaded cert)
 * worker_id  : used in log messages and HTTP headers
 */
static void handle_incoming_fd(int conn_fd, WOLFSSL_CTX *wctx, int worker_id)
{
    fprintf(stderr,
            "\n[worker-%d] ─── Receiving fd from gateway ───\n", worker_id);

    /* 1. Receive the file descriptor and TLS serial state */
    int              client_fd = -1;
    tlspeek_serial_t serial;
    memset(&serial, 0, sizeof(serial));

    if (recvfd_with_state(conn_fd, &client_fd, &serial, sizeof(serial)) != 0) {
        fprintf(stderr, "[worker-%d] recvfd_with_state failed\n", worker_id);
        return;
    }

    fprintf(stderr,
            "[worker-%d] Received client_fd=%d from gateway\n",
            worker_id, client_fd);

    /* 2. Validate magic */
    if (serial.magic != TLSPEEK_MAGIC) {
        fprintf(stderr,
                "[worker-%d] ERROR: bad magic 0x%08X (expected 0x%08X)\n",
                worker_id, serial.magic, TLSPEEK_MAGIC);
        close(client_fd);
        return;
    }
    fprintf(stderr, "[worker-%d] Serial magic OK. cipher=0x%04X (blob_sz=%u, request_len=%d)\n",
            worker_id, serial.cipher_suite, serial.blob_sz, serial.request_len);

    /* 3. Create a new wolfSSL session and RESTORE the exported session state.
     *
     * In the zero-copy approach, the gateway only peeked at the data.
     * The worker MUST call wolfSSL_read() to actually consume the records.
     */
    WOLFSSL *ssl = wolfSSL_new(wctx);
    if (!ssl) {
        fprintf(stderr, "[worker-%d] wolfSSL_new failed\n", worker_id);
        close(client_fd);
        return;
    }

    /* Pattern: Set FD, Import, then Set FD again as a correction */
    wolfSSL_set_fd(ssl, client_fd);

    fprintf(stderr, "[worker-%d] Restoring TLS session state...\n", worker_id);
    if (tlspeek_restore(ssl, &serial) != 0) {
        fprintf(stderr, "[worker-%d] tlspeek_restore failed\n", worker_id);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }
    
    /* Correction post-import: ensure the restored session uses the socket FD */
    wolfSSL_set_fd(ssl, client_fd);

    /* 4. Read the HTTP request from the encrypted session.
     *
     * IMPORTANT: the gateway only PEEKED at this data using tls_read_peek().
     * The kernel buffer still contains the original records. wolfSSL_read()
     * will now actually consume those records from the socket.
     */
    char request[8192];
    memset(request, 0, sizeof(request));

    fprintf(stderr, "[worker-%d] wolfSSL_read()...\n", worker_id);
    int req_len = wolfSSL_read(ssl, request, (int)sizeof(request) - 1);
    if (req_len <= 0) {
        int err = wolfSSL_get_error(ssl, req_len);
        char errstr[80];
        wolfSSL_ERR_error_string((unsigned long)err, errstr);
        fprintf(stderr, "[worker-%d] wolfSSL_read failed: %d (%s)\n", worker_id, err, errstr);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }
    request[req_len] = '\0';
    fprintf(stderr, "[worker-%d] Successfully read %d bytes from kernel buffer: %.50s...\n", 
            worker_id, req_len, request);

    /* 5. Handle the request and build a response */
    char *response = handle_http_request(request, req_len, worker_id);
    if (!response) {
        fprintf(stderr, "[worker-%d] handle_http_request: malloc failed\n", worker_id);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }

    /* 6. Send HTTPS response directly to the client */
    fprintf(stderr, "[worker-%d] wolfSSL_write() response...\n", worker_id);
    int resp_len = (int)strlen(response);
    int written  = wolfSSL_write(ssl, response, resp_len);
    if (written != resp_len) {
        int err = wolfSSL_get_error(ssl, written);
        char errstr[80];
        wolfSSL_ERR_error_string((unsigned long)err, errstr);
        fprintf(stderr,
                "[worker-%d] wolfSSL_write: wrote %d of %d bytes, err=%s\n",
                worker_id, written, resp_len, errstr);
    } else {
        fprintf(stderr,
                "[worker-%d] Sent %d bytes response directly to client\n",
                worker_id, written);
    }

    free(response);

    /* 7. Clean shutdown */
    fprintf(stderr, "[worker-%d] wolfSSL_shutdown()\n", worker_id);
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_fd);

    fprintf(stderr,
            "[worker-%d] ─── Request handled, connection closed ───\n\n",
            worker_id);
}

/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <worker_id> [cert.crt] [cert.key]\n"
                "Example: ./worker 0\n"
                "         ./worker 1 certs/server.crt certs/server.key\n",
                argv[0]);
        return 1;
    }

    int         worker_id  = atoi(argv[1]);
    /* Optional cert/key for worker-side wolfSSL context.
     * The worker needs a valid CTX to create WOLFSSL* objects.
     * For prototype simplicity the cert is not verified on the worker
     * because the worker is the server-side of an existing connection.
     * We only need the CTX to allocate the WOLFSSL* session object. */
    const char *cert_file  = (argc >= 4) ? argv[2] : "certs/server.crt";
    const char *key_file   = (argc >= 4) ? argv[3] : "certs/server.key";

    fprintf(stderr,
            "[worker-%d] Starting (cert=%s key=%s)\n",
            worker_id, cert_file, key_file);

    signal(SIGPIPE, SIG_IGN);

    /* ── wolfSSL init ── */
    wolfSSL_Init();
    wolfSSL_Debugging_ON();

    /*
     * Use server method — the worker is acting as the server side of
     * an existing (already handshaked) TLS connection.
     * We won't call wolfSSL_accept() — instead tlspeek_restore() will
     * inject the session keys directly.
     */
    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) {
        fprintf(stderr, "[worker-%d] wolfSSL_CTX_new failed\n", worker_id);
        return 1;
    }

    /* Load the server certificate (needed to init a valid CTX) */
    if (wolfSSL_CTX_use_certificate_file(wctx, cert_file,
                                          SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr,
                "[worker-%d] Warning: could not load cert %s — "
                "continuing anyway (cert not needed for key injection)\n",
                worker_id, cert_file);
    }
    if (wolfSSL_CTX_use_PrivateKey_file(wctx, key_file,
                                         SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr,
                "[worker-%d] Warning: could not load key %s\n",
                worker_id, key_file);
    }

    /* ── Create Unix socket ── */
    char sock_path[64];
    snprintf(sock_path, sizeof(sock_path), "/tmp/worker_%d.sock", worker_id);

    int listen_fd = unix_server_socket(sock_path, UNIX_BACKLOG);
    if (listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    fprintf(stderr,
            "[worker-%d] Ready. Waiting for connections on %s\n\n",
            worker_id, sock_path);

    /* ── Main loop: accept gateway connections ── */
    while (1) {
        /*
         * Each accept() gives us a new connection FROM THE GATEWAY
         * (not from the client directly).  The gateway sends one fd
         * per HTTP request over this connection.
         */
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) continue;

        handle_incoming_fd(conn_fd, wctx, worker_id);

        /* Close the gateway→worker control connection after handling */
        close(conn_fd);
    }

    /* Cleanup (unreachable in prototype) */
    close(listen_fd);
    unlink(sock_path);
    wolfSSL_CTX_free(wctx);
    wolfSSL_Cleanup();

    return 0;
}
