#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>

#define DEFAULT_PORT 8443
#define CERT_FILE "../libtlspeek/certs/server.crt"
#define KEY_FILE  "../libtlspeek/certs/server.key"

void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        while (waitpid(-1, NULL, WNOHANG) > 0);
    } else {
        exit(0);
    }
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "wolfSSL_CTX_new error\n");
        return 1;
    }

    if (wolfSSL_CTX_use_certificate_file(ctx, CERT_FILE, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Error loading cert\n");
        return 1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, KEY_FILE, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Error loading key\n");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    servAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        perror("bind failed");
        return 1;
    }

    listen(sockfd, 1024);
    printf("Direct TLS Server (Multi-Process) listening on port %d\n", port);

    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int client_fd = accept(sockfd, (struct sockaddr*)&clientAddr, &clientLen);
        if (client_fd < 0) continue;

        if (fork() == 0) {
            close(sockfd);
            WOLFSSL* ssl = wolfSSL_new(ctx);
            wolfSSL_set_fd(ssl, client_fd);

            if (wolfSSL_accept(ssl) == SSL_SUCCESS) {
                char buffer[1024];
                int n = wolfSSL_read(ssl, buffer, sizeof(buffer)-1);
                if (n > 0) {
                    const char* response = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 22\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Hello from Direct Mode";
                    wolfSSL_write(ssl, response, (int)strlen(response));
                }
            }
            wolfSSL_shutdown(ssl);
            wolfSSL_free(ssl);
            close(client_fd);
            exit(0);
        }
        close(client_fd);
    }

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}
