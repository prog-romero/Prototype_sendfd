#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <errno.h>
#include "common.h"

/* 
 * Classic Proxy (Baseline) : 
 * Simule le comportement d'un Reverse Proxy standard (type Nginx).
 * Il déchiffre le TLS du client et relaie les données brutes sur un UDS.
 */

void bridge(int client_ssl_fd, WOLFSSL* ssl, int worker_fd) {
    char buf[4096];
    int n;
    
    /* 1. Lire la requête du client (Déchiffrée par wolfSSL) */
    n = wolfSSL_read(ssl, buf, sizeof(buf));
    if (n <= 0) return;
    
    /* 2. Envoyer au worker via UDS (Simple write) */
    write(worker_fd, buf, n);
    
    /* 3. Lire la réponse du worker */
    n = read(worker_fd, buf, sizeof(buf));
    if (n <= 0) return;
    
    /* 4. Renvoyer au client (Chiffrée par wolfSSL) */
    wolfSSL_write(ssl, buf, n);
}

int main() {
    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8444), .sin_addr.s_addr = INADDR_ANY };
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(sockfd, 10);

    printf("[Baseline] Proxy classique sur port 8444. Redirection vers les Workers UDS.\n");

    while (1) {
        int client_fd = accept(sockfd, NULL, NULL);
        WOLFSSL* ssl = wolfSSL_new(ctx);
        wolfSSL_set_fd(ssl, client_fd);

        if (wolfSSL_accept(ssl) == SSL_SUCCESS) {
            /* On se connecte au worker SUM par défaut pour le test de tunnel */
            int worker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un uds_addr = { .sun_family = AF_UNIX };
            strcpy(uds_addr.sun_path, UDS_PATH_SUM);
            
            if (connect(worker_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) == 0) {
                bridge(client_fd, ssl, worker_fd);
            }
            close(worker_fd);
        }
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_fd);
    }
    return 0;
}
