/*
 * gateway.c — Main gateway process.
 *
 * Listens on TCP port 8443 (HTTPS).
 * Performs TLS 1.3 handshake (wolfSSL).
 * Calls tls_read_peek() to inspect HTTP headers WITHOUT consuming the
 * kernel buffer.
 * Routes to a worker and transfers the fd via SCM_RIGHTS.
 *
 * Usage:  ./gateway <port> <cert.crt> <cert.key> <num_workers>
 * Example: ./gateway 8443 certs/server.crt certs/server.key 2
 */

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>


#include "tlspeek.h"
#include "tlspeek_serial.h"
#include "sendfd.h"
#include "unix_socket.h"
#include "router.h"

#define MAX_WORKERS   8
#define LISTEN_BACKLOG 16

/* ─────────────────────────────────────────────────────────────────────────── */
/* TCP server socket setup                                                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static int create_tcp_listener(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("[gw] socket"); return -1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[gw] bind"); close(sock); return -1;
    }
    if (listen(sock, LISTEN_BACKLOG) < 0) {
        perror("[gw] listen"); close(sock); return -1;
    }

    fprintf(stderr, "[gw] TCP listener on port %d (fd=%d)\n", port, sock);
    return sock;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Per-connection handler                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

static void handle_connection(
    int          client_fd,
    WOLFSSL_CTX *wctx,
    int          n_workers)
{
    fprintf(stderr,
            "\n[gw] ─── New connection: client_fd=%d ───\n", client_fd);

    /* 1. Create wolfSSL session */
    WOLFSSL *ssl = wolfSSL_new(wctx);
    if (!ssl) {
        fprintf(stderr, "[gw] wolfSSL_new failed\n");
        close(client_fd);
        return;
    }
    wolfSSL_set_fd(ssl, client_fd);

    /* 2. Allocate tlspeek context and associate with this ssl session.
     *    The keylog callback retrieves this pointer via wolfSSL_get_ex_data(ssl,0). */
    tlspeek_ctx_t peek_ctx;
    memset(&peek_ctx, 0, sizeof(peek_ctx));
    wolfSSL_set_ex_data(ssl, 0, &peek_ctx);
    peek_ctx.tcp_fd = client_fd;
    peek_ctx.ssl    = ssl;

    /* Register the TLS 1.3 secret callback on this session */
    wolfSSL_set_tls13_secret_cb(ssl, tlspeek_keylog_cb, &peek_ctx);

    /* 3. TLS handshake — keylog callback fires here automatically */
    fprintf(stderr, "[gw] Performing TLS handshake...\n");
    int ret = wolfSSL_accept(ssl);
    if (ret != SSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, ret);
        char errstr[80];
        wolfSSL_ERR_error_string((unsigned long)err, errstr);
        fprintf(stderr, "[gw] wolfSSL_accept failed: %d (%s)\n", err, errstr);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }
    fprintf(stderr, "[gw] TLS handshake complete — cipher: %s\n",
            wolfSSL_get_cipher_name(ssl));

    /* 4. Verify keys were captured by the keylog callback */
    if (!peek_ctx.keys_ready) {
        fprintf(stderr,
                "[gw] ERROR: keylog callback did not fire — "
                "keys_ready=0. Ensure wolfssl was built with --enable-opensslextra.\n");
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }

    /* Determine actual cipher suite from wolfSSL */
    const char *cipher_name = wolfSSL_get_cipher_name(ssl);
    if (cipher_name) {
        if (strstr(cipher_name, "CHACHA20"))
            peek_ctx.cipher_suite = TLSPEEK_CHACHA20_POLY;
        else if (strstr(cipher_name, "AES-128"))
            peek_ctx.cipher_suite = TLSPEEK_AES_128_GCM;
        else
            peek_ctx.cipher_suite = TLSPEEK_AES_256_GCM;
    }

    /* 5. Zero-Copy TLS Read Peek — inspect headers WITHOUT consuming the kernel buffer */
    uint8_t headers_buf[TLSPEEK_MAX_REQUEST_SZ];
    memset(headers_buf, 0, sizeof(headers_buf));

    fprintf(stderr, "[gw] Calling tls_read_peek()...\n");
    int headers_len = tls_read_peek(&peek_ctx, headers_buf,
                                     sizeof(headers_buf) - 1);
    if (headers_len <= 0) {
        fprintf(stderr, "[gw] tls_read_peek() failed\n");
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }
    headers_buf[headers_len] = '\0';
    fprintf(stderr, "[gw] tls_read_peek() returned %d bytes\n", headers_len);
    fprintf(stderr, "[gw] Peeked Request: %.200s\n", headers_buf);

    /* 6. Routing decision */
    int worker_id = route_request((char *)headers_buf, headers_len);
    if (worker_id < 0 || worker_id >= n_workers) {
        fprintf(stderr, "[gw] worker_id %d out of range — defaulting to 0\n", worker_id);
        worker_id = 0;
    }
    fprintf(stderr, "[gw] Routing to worker %d\n", worker_id);

    /* 7. Export TLS session state */
    tlspeek_serial_t serial;
    memset(&serial, 0, sizeof(serial));
    serial.magic = TLSPEEK_MAGIC;
    serial.cipher_suite = peek_ctx.cipher_suite;

    serial.blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    int export_ret = wolfSSL_tls_export(ssl, serial.tls_blob, &serial.blob_sz);
    if (export_ret < 0) {
        fprintf(stderr, "[gw] wolfSSL_tls_export failed (ret=%d)\n", export_ret);
        wolfSSL_free(ssl);
        close(client_fd);
        return;
    }
    fprintf(stderr, "[gw] wolfSSL_tls_export OK: %u bytes\n", serial.blob_sz);

    /* We still fill the request_len to 0 or leave it, the worker will read from socket */
    serial.request_len = 0; 

    /* 8. Detach wolfSSL from the socket so we can free it without sending alerts */
    wolfSSL_set_quiet_shutdown(ssl, 1);
    wolfSSL_set_fd(ssl, -1);
    wolfSSL_free(ssl);
    tlspeek_free(&peek_ctx);

    /* 9. Transfer fd + serialized state to worker.
     *    Connect on-demand to the worker's Unix socket (matches Romero_New pattern).
     */
    char sock_path[64];
    snprintf(sock_path, sizeof(sock_path), "/tmp/worker_%d.sock", worker_id);
    
    int uds_fd = unix_client_connect(sock_path);
    if (uds_fd < 0) {
        fprintf(stderr, "[gw] Failed to connect to worker %d at %s\n", worker_id, sock_path);
        close(client_fd);
        return;
    }

    if (sendfd_with_state(uds_fd, client_fd, &serial, sizeof(serial)) != 0) {
        fprintf(stderr, "[gw] sendfd_with_state failed\n");
        close(uds_fd);
        /* client_fd is closed by sendfd_with_state even on partial failure usually, 
           but let's be safe if it didn't even call sendmsg */
        return;
    }

    close(uds_fd);
    fprintf(stderr, "[gw] ─── Connection handed off to worker %d ───\n\n", worker_id);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* main()                                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <port> <cert.crt> <cert.key> <num_workers>\n"
                "Example: ./gateway 8443 certs/server.crt certs/server.key 2\n",
                argv[0]);
        return 1;
    }

    int         port       = atoi(argv[1]);
    const char *cert_file  = argv[2];
    const char *key_file   = argv[3];
    int         n_workers  = atoi(argv[4]);

    if (n_workers < 1 || n_workers > MAX_WORKERS) {
        fprintf(stderr, "[gw] n_workers must be 1..%d\n", MAX_WORKERS);
        return 1;
    }

    /* Ignore SIGPIPE — handle broken connections gracefully */
    signal(SIGPIPE, SIG_IGN);

    /* ── wolfSSL initialisation ── */
    wolfSSL_Init();
    wolfSSL_Debugging_ON();  /* verbose wolfSSL logs to stderr */

    /* Use compatibility method and restrict via SetMinVersion later */
    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) {
        fprintf(stderr, "[gw] wolfSSL_CTX_new failed\n");
        return 1;
    }

    /* Restrict to TLS 1.3 ONLY */
    /* Restrict to TLS 1.3 ONLY using wolfSSL native naming */
    wolfSSL_CTX_SetMinVersion(wctx, WOLFSSL_TLSV1_3);
    wolfSSL_CTX_set_max_proto_version(wctx, WOLFSSL_TLSV1_3);

    /* Load certificate and private key */
    if (wolfSSL_CTX_use_certificate_file(wctx, cert_file,
                                          SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr, "[gw] Failed to load certificate: %s\n", cert_file);
        wolfSSL_CTX_free(wctx);
        return 1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(wctx, key_file,
                                         SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr, "[gw] Failed to load private key: %s\n", key_file);
        wolfSSL_CTX_free(wctx);
        return 1;
    }
    fprintf(stderr, "[gw] Certificate and key loaded\n");

    /* 
     * Keylog callback: The newer byte-based secret callback is now 
     * installed on each session in handle_connection() using 
     * wolfSSL_set_tls13_secret_cb().
     */
    fprintf(stderr, "[gw] Session-level secret callback enabled\n");

    /* ── Connect to worker Unix sockets ── 
     * In the zero-copy pipeline, we connect on-demand in handle_connection 
     * to match the faad_optimization-Romero_New pattern.
     */

    /* ── TCP listener ── */
    int tcp_listen_fd = create_tcp_listener(port);
    if (tcp_listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    fprintf(stderr,
            "\n[gw] Gateway ready. Listening on :%d, %d worker(s)\n\n",
            port, n_workers);

    /* ── Main accept() loop (single-threaded, no threads per spec) ── */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = accept(tcp_listen_fd,
                               (struct sockaddr *)&client_addr,
                               &addrlen);
        if (client_fd < 0) {
            perror("[gw] accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        fprintf(stderr, "[gw] Accepted TCP connection from %s:%d (fd=%d)\n",
                client_ip, ntohs(client_addr.sin_port), client_fd);

        handle_connection(client_fd, wctx, n_workers);
    }

    /* Cleanup (unreachable in prototype) */
    close(tcp_listen_fd);
    wolfSSL_CTX_free(wctx);
    wolfSSL_Cleanup();

    return 0;
}
