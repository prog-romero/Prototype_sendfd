/*
 * direct_tls_server.c — Standalone HTTPS Server (Baseline)
 * Harmonized with the Prototype's worker logic for fair comparison.
 */

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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <errno.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "handler.h"

#define CERT_FILE "../libtlspeek/certs/server.crt"
#define KEY_FILE  "../libtlspeek/certs/server.key"
#define BACKLOG 16384
#define NUM_LISTENERS 2

void handle_connection(int client_fd, WOLFSSL_CTX* ctx, int worker_id) {
    /* Set TCP_NODELAY for lower latency */
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    WOLFSSL* ssl = wolfSSL_new(ctx);
    if (!ssl) {
        close(client_fd);
        return;
    }

    wolfSSL_set_fd(ssl, client_fd);

    if (wolfSSL_accept(ssl) == SSL_SUCCESS) {
        while (1) {
            char request[8192];
            int req_len = wolfSSL_read(ssl, request, (int)sizeof(request) - 1);
            if (req_len <= 0) break;
            request[req_len] = '\0';

            /* Use the same handler as the prototype */
            char *response = handle_http_request(request, req_len, worker_id);
            if (!response) break;

            int resp_len = (int)strlen(response);
            int written  = wolfSSL_write(ssl, response, resp_len);
            free(response);
            if (written != resp_len) break;
        }
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_fd);
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 8445;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (ctx == NULL) return 1;

    if (wolfSSL_CTX_use_certificate_file(ctx, CERT_FILE, SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(ctx, KEY_FILE, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Error loading certs\n");
        return 1;
    }

    printf("[direct-tls] Starting harmonized baseline on port %d (%d listeners)...\n", port, NUM_LISTENERS);

    for (int i = 0; i < NUM_LISTENERS; i++) {
        if (fork() == 0) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

            struct sockaddr_in servAddr;
            memset(&servAddr, 0, sizeof(servAddr));
            servAddr.sin_family = AF_INET;
            servAddr.sin_port = htons(port);
            servAddr.sin_addr.s_addr = INADDR_ANY;

            if (bind(sockfd, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
                perror("bind failed");
                exit(1);
            }
            listen(sockfd, BACKLOG);

            while (1) {
                struct sockaddr_in clientAddr;
                socklen_t clientLen = sizeof(clientAddr);
                int client_fd = accept(sockfd, (struct sockaddr*)&clientAddr, &clientLen);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    continue;
                }

                if (fork() == 0) {
                    close(sockfd);
                    handle_connection(client_fd, ctx, i);
                    exit(0);
                }
                close(client_fd);
            }
            exit(0);
        }
    }

    while (wait(NULL) > 0);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}
