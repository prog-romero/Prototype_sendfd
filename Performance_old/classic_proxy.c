#line 1 "/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Migration_TLS_2/Migration_TLS/classic_proxy.c"
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include "common.h"

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main() {
    signal(SIGCHLD, sigchld_handler);
    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8444), .sin_addr.s_addr = INADDR_ANY };
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(sockfd, 1024);

    printf("[Baseline] Proxy classique prêt (Multi-process V5)\n");

    while (1) {
        int client_fd = accept(sockfd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fork() == 0) {
            close(sockfd);
            WOLFSSL* ssl = wolfSSL_new(ctx);
            wolfSSL_set_fd(ssl, client_fd);

            if (wolfSSL_accept(ssl) == SSL_SUCCESS) {
                int worker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                struct sockaddr_un uds_addr = { .sun_family = AF_UNIX };
                strcpy(uds_addr.sun_path, UDS_PATH_SUM);
                
                if (connect(worker_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) == 0) {
                    while (1) {
                        char req_buf[MAX_REQUEST_SZ] = {0};
                        int n = wolfSSL_read(ssl, req_buf, sizeof(req_buf)-1);
                        if (n <= 0) break;

                        // Relay to worker
                        if (write(worker_fd, req_buf, n) <= 0) break;
                        
                        // Buffer for safety (worker response might be multiple chunks in some cases)
                        char resp_buf[2048] = {0};
                        int rn = read(worker_fd, resp_buf, sizeof(resp_buf)-1);
                        if (rn <= 0) break;
                        
                        // Relay back to client
                        if (wolfSSL_write(ssl, resp_buf, rn) <= 0) break;
                    }
                }
                close(worker_fd);
            }
            wolfSSL_free(ssl);
            close(client_fd);
            exit(0);
        }
        close(client_fd);
    }
    return 0;
}
