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
#include <sys/wait.h>
#include <errno.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "tlspeek.h"
#include "tlspeek_serial.h"
#include "sendfd.h"
#include "unix_socket.h"
#include "handler.h"

#define UNIX_BACKLOG  4096
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
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    tlspeek_serial_t serial;
    memset(&serial, 0, sizeof(serial));

    struct iovec iov = {
        .iov_base = &serial,
        .iov_len  = sizeof(serial),
    };

    struct msghdr msg = {
        .msg_name       = NULL,
        .msg_namelen    = 0,
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
        .msg_flags      = 0,
    };

#ifndef QUIET
    fprintf(stderr, "[worker-%d] Waiting for handoff (Migration OR Proxy)...\n", worker_id);
#endif

    ssize_t n = recvmsg(conn_fd, &msg, 0);
    if (n <= 0) {
        if (n < 0) perror("recvmsg");
        return;
    }

    int client_fd = -1;
    int is_migration = 0;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        client_fd = *((int *)CMSG_DATA(cmsg));
        is_migration = 1;
    } else {
        /* Plain HTTP Proxy Mode: the client_fd IS the conn_fd */
        client_fd = conn_fd;
    }

    if (is_migration) {
#ifndef QUIET
        fprintf(stderr, "[worker-%d] MODE: TLS Migration (client_fd=%d)\n", worker_id, client_fd);
#endif
        /* 1. RESTORE the exported session state. */
        WOLFSSL *ssl = wolfSSL_new(wctx);
        if (!ssl) {
            close(client_fd);
            return;
        }

        wolfSSL_set_fd(ssl, client_fd);
        if (tlspeek_restore(ssl, &serial) != 0) {
            fprintf(stderr, "[worker-%d] tlspeek_restore failed\n", worker_id);
            wolfSSL_free(ssl);
            close(client_fd);
            return;
        }
        wolfSSL_set_fd(ssl, client_fd);

        /* 2. Handle Keep-Alive requests */
        while (1) {
            char request[8192];
            int req_len = wolfSSL_read(ssl, request, (int)sizeof(request) - 1);
            if (req_len <= 0) break;
            request[req_len] = '\0';

            char *response = handle_http_request(request, req_len, worker_id);
            if (!response) break;
            int resp_len = (int)strlen(response);
            int written  = wolfSSL_write(ssl, response, resp_len);
            free(response);
            if (written != resp_len) break;
        }
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_fd);
    } else {
#ifndef QUIET
        fprintf(stderr, "[worker-%d] MODE: Nginx Proxy (Direct UDS)\n", worker_id);
#endif
        /* Plain HTTP Logic (Nginx Proxy) */
        while (1) {
            char request[8192];
            int req_len;
            
            if (n > 0) {
                /* Use initial data from recvmsg */
                memcpy(request, &serial, (n > (ssize_t)sizeof(request)-1) ? sizeof(request)-1 : (size_t)n);
                req_len = n;
                n = 0; /* Reset for next iteration */
            } else {
                req_len = read(client_fd, request, sizeof(request) - 1);
            }
            
            if (req_len <= 0) break;
            request[req_len] = '\0';

            char *response = handle_http_request(request, req_len, worker_id);
            if (!response) break;
            int resp_len = (int)strlen(response);
            int written  = write(client_fd, response, resp_len);
            free(response);
            if (written != resp_len) break;
        }
        /* In Proxy mode, the socket is managed by Nginx/Worker loop, we close it here as it was accepted */
        close(client_fd);
    }
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

    /* Ignore SIGPIPE and SIGCHLD */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* ── wolfSSL init ── */
    wolfSSL_Init();
    /* wolfSSL_Debugging_ON(); */

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
    snprintf(sock_path, sizeof(sock_path), "worker_%d.sock", worker_id);

    int listen_fd = unix_server_socket(sock_path, UNIX_BACKLOG);
    if (listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    fprintf(stderr,
            "[worker-%d] Ready. Waiting for connections on %s\n\n",
            worker_id, sock_path);

    /* ── Main loop: accept gateway connections (On-demand fork) ── */
    signal(SIGCHLD, SIG_IGN);
    while (1) {
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* Child process */
            close(listen_fd);
            handle_incoming_fd(conn_fd, wctx, worker_id);
            close(conn_fd);
            
            wolfSSL_CTX_free(wctx);
            wolfSSL_Cleanup();
            exit(0);
        } else if (pid > 0) {
            /* Parent process */
            close(conn_fd);
        } else {
            perror("[worker] fork");
            close(conn_fd);
        }
    }

    /* Cleanup (unreachable in prototype) */
    close(listen_fd);
    unlink(sock_path);
    wolfSSL_CTX_free(wctx);
    wolfSSL_Cleanup();

    return 0;
}
