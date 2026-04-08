/*
 * MB-1 Server for Raspberry Pi
 * 
 * Standalone server that listens on 0.0.0.0:19445 and accepts TLS connections.
 * This is deployed on the Raspberry Pi and runs indefinitely.
 * 
 * Usage: ./mb1_server_pi
 * 
 * The server:
 * - Listens on 0.0.0.0:19445 (all interfaces)
 * - Accepts TLS connections
 * - Exchanges test messages with clients
 * - Continues running until killed with Ctrl+C or pkill
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

#include <wolfssl/ssl.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration */
/* ─────────────────────────────────────────────────────────────────────────── */

#define BENCHMARK_PORT 19445
#define BENCHMARK_ADDR "0.0.0.0"  /* Listen on all interfaces */
#define TIMEOUT_SEC 10
#define TEST_MESSAGE "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
#define TEST_RESPONSE "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nOK\r\n"

/* Certificate paths - modify if needed for Pi */
#define CERT_FILE "./wolfssl_certs/server-cert.der"
#define KEY_FILE "./wolfssl_certs/server-key.der"

/* ─────────────────────────────────────────────────────────────────────────── */
/* Global state */
/* ─────────────────────────────────────────────────────────────────────────── */

static volatile int server_running = 1;
static WOLFSSL_CTX *g_server_ctx = NULL;
static int g_server_socket = -1;
static unsigned long g_connection_count = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Signal handler */
/* ─────────────────────────────────────────────────────────────────────────── */

static void signal_handler(int sig) {
    fprintf(stderr, "[SERVER] Signal %d received. Shutting down.\n", sig);
    fprintf(stderr, "[SERVER] Accepted %lu connections total.\n", g_connection_count);
    server_running = 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main server loop */
/* ─────────────────────────────────────────────────────────────────────────── */

int main() {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    WOLFSSL *ssl;
    char buffer[256];
    int numbytes;
    struct timeval timeout;

    fprintf(stderr, "[SERVER] Initializing wolfSSL...\n");
    wolfSSL_library_init();

    /* Create server context */
    fprintf(stderr, "[SERVER] Creating TLS server context...\n");
    g_server_ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!g_server_ctx) {
        fprintf(stderr, "[ERROR] Failed to create server SSL context\n");
        return 1;
    }

    /* Load server certificate and key from files */
    fprintf(stderr, "[SERVER] Loading certificate from %s\n", CERT_FILE);
    if (wolfSSL_CTX_use_certificate_file(g_server_ctx, CERT_FILE, SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server certificate from %s\n", CERT_FILE);
        fprintf(stderr, "[ERROR] Make sure: scp -r wolfssl/certs pi@<PI_IP>:~/wolfssl_certs\n");
        wolfSSL_CTX_free(g_server_ctx);
        return 1;
    }

    fprintf(stderr, "[SERVER] Loading private key from %s\n", KEY_FILE);
    if (wolfSSL_CTX_use_PrivateKey_file(g_server_ctx, KEY_FILE, SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server key from %s\n", KEY_FILE);
        wolfSSL_CTX_free(g_server_ctx);
        return 1;
    }

    /* Create server socket */
    fprintf(stderr, "[SERVER] Creating server socket...\n");
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("[ERROR] socket");
        wolfSSL_CTX_free(g_server_ctx);
        return 1;
    }

    /* Allow socket reuse */
    int reuse = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Bind to all interfaces */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(BENCHMARK_ADDR);

    fprintf(stderr, "[SERVER] Binding to 0.0.0.0:%d\n", BENCHMARK_PORT);
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] bind");
        close(server_sock);
        wolfSSL_CTX_free(g_server_ctx);
        return 1;
    }

    /* Listen for incoming connections */
    if (listen(server_sock, 128) < 0) {
        perror("[ERROR] listen");
        close(server_sock);
        wolfSSL_CTX_free(g_server_ctx);
        return 1;
    }

    fprintf(stderr, "[SERVER] ✓ Server listening on 0.0.0.0:%d\n", BENCHMARK_PORT);
    fprintf(stderr, "[SERVER] Waiting for connections (Ctrl+C to stop)...\n");
    fprintf(stderr, "[SERVER] ─────────────────────────────────────────────────\n");

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_server_socket = server_sock;

    /* Accept connections loop */
    client_addr_len = sizeof(client_addr);
    while (server_running) {
        /* Accept connection with timeout */
        fd_set readfds;
        struct timeval select_timeout;
        
        FD_ZERO(&readfds);
        FD_SET(server_sock, &readfds);
        
        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 0;
        
        int select_ret = select(server_sock + 1, &readfds, NULL, NULL, &select_timeout);
        if (select_ret == 0) {
            /* Timeout - check if we should continue */
            continue;
        }
        if (select_ret < 0) {
            if (errno == EINTR) continue;  /* Interrupted by signal */
            perror("[ERROR] select");
            break;
        }

        /* Accept connection */
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            if (errno == EINTR) continue;  /* Interrupted by signal */
            perror("[ERROR] accept");
            continue;
        }

        g_connection_count++;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        fprintf(stderr, "[SERVER] Connection #%lu from %s:%d\n", 
                g_connection_count,
                client_ip,
                ntohs(client_addr.sin_port));

        /* Set socket timeout */
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        /* Create SSL object */
        ssl = wolfSSL_new(g_server_ctx);
        if (!ssl) {
            fprintf(stderr, "[ERROR] Failed to create SSL object\n");
            close(client_sock);
            continue;
        }

        /* Perform TLS handshake */
        wolfSSL_set_fd(ssl, client_sock);
        int ret = wolfSSL_accept(ssl);
        if (ret != SSL_SUCCESS) {
            int err = wolfSSL_get_error(ssl, ret);
            fprintf(stderr, "[WARN] TLS handshake failed: error %d\n", err);
            wolfSSL_free(ssl);
            close(client_sock);
            continue;
        }

        fprintf(stderr, "[SERVER]   ✓ TLS handshake successful (TLS 1.3)\n");

        /* Read test message from client */
        memset(buffer, 0, sizeof(buffer));
        numbytes = wolfSSL_read(ssl, (unsigned char *)buffer, sizeof(buffer) - 1);
        if (numbytes > 0) {
            fprintf(stderr, "[SERVER]   ✓ Received %d bytes from client\n", numbytes);
        }

        /* Send test response */
        ret = wolfSSL_write(ssl, (unsigned char *)TEST_RESPONSE, strlen(TEST_RESPONSE));
        if (ret > 0) {
            fprintf(stderr, "[SERVER]   ✓ Sent response to client\n");
        } else {
            fprintf(stderr, "[WARN] Failed to send response\n");
        }

        /* Shutdown SSL connection */
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_sock);

        fprintf(stderr, "[SERVER]   ✓ Connection closed\n");
    }

    /* Cleanup */
    fprintf(stderr, "[SERVER] Cleaning up...\n");
    close(server_sock);
    wolfSSL_CTX_free(g_server_ctx);
    wolfSSL_Cleanup();

    fprintf(stderr, "[SERVER] Server stopped.\n");
    return 0;
}
