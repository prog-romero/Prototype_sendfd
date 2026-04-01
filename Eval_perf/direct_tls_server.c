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
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define MAX_EVENTS 2048
#define MAX_FDS 65536

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "handler.h"

#define CERT_FILE "../libtlspeek/certs/server.crt"
#define KEY_FILE  "../libtlspeek/certs/server.key"
#define BACKLOG 16384
#define NUM_LISTENERS 2

// handle_connection removed, integrated into epoll

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

            set_nonblocking(sockfd);
            int epfd = epoll_create1(0);
            struct epoll_event ev, events[MAX_EVENTS];
            ev.events = EPOLLIN;
            ev.data.fd = sockfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
            
            WOLFSSL* ssl_map[MAX_FDS] = {0};

            while (1) {
                int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
                if (nfds < 0 && errno == EINTR) continue;

                for (int j = 0; j < nfds; j++) {
                    int fd = events[j].data.fd;

                    if (fd == sockfd) {
                        while (1) {
                            struct sockaddr_in clientAddr;
                            socklen_t clientLen = sizeof(clientAddr);
                            int client_fd = accept(sockfd, (struct sockaddr*)&clientAddr, &clientLen);
                            if (client_fd < 0) break;
                            
                            set_nonblocking(client_fd);
                            int nodelay = 1;
                            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                            WOLFSSL* ssl = wolfSSL_new(ctx);
                            if (!ssl) {
                                close(client_fd);
                                continue;
                            }
                            wolfSSL_set_fd(ssl, client_fd);
                            ssl_map[client_fd] = ssl;

                            struct epoll_event client_ev;
                            client_ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                            client_ev.data.fd = client_fd;
                            epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
                        }
                    } else {
                        WOLFSSL *ssl = ssl_map[fd];
                        if (!ssl) continue;

                        if (!wolfSSL_is_init_finished(ssl)) {
                            int ret = wolfSSL_accept(ssl);
                            if (ret != SSL_SUCCESS) {
                                int err = wolfSSL_get_error(ssl, ret);
                                if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) continue;
                                goto close_client;
                            }
                            continue;
                        }

                        char request[8192];
                        int req_len = wolfSSL_read(ssl, request, sizeof(request) - 1);
                        if (req_len > 0) {
                            request[req_len] = '\0';
                            char *response = handle_http_request(request, req_len, i);
                            if (response) {
                                int resp_len = strlen(response);
                                int total_written = 0;
                                while (total_written < resp_len) {
                                    int written = wolfSSL_write(ssl, response + total_written, resp_len - total_written);
                                    if (written > 0) {
                                        total_written += written;
                                    } else {
                                        int err = wolfSSL_get_error(ssl, written);
                                        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) break;
                                        free(response);
                                        goto close_client;
                                    }
                                }
                                free(response);
                            } else goto close_client;
                        } else if (req_len < 0) {
                            int err = wolfSSL_get_error(ssl, req_len);
                            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) continue;
                            goto close_client;
                        } else {
                            goto close_client;
                        }
                        continue;
                        
                    close_client:
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        wolfSSL_free(ssl);
                        ssl_map[fd] = NULL;
                        close(fd);
                    }
                }
            }
            exit(0);
        }
    }

    while (wait(NULL) > 0);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}
