/*
 * MB-2 Server - TLS read_peek() vs wolfSSL_read() overhead measurement
 * Runs on Raspberry Pi
 * 
 * Sends varying payload sizes through TLS connection
 * Allows client to measure peek() overhead
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

#define MB2_PORT 19446
#define BACKLOG 1
#define TIMEOUT_SEC 30

/* Test payload sizes - 15 samples [test data points] logarithmically distributed [spread out on a log scale] */
#define N_SIZES 15
#define MAX_PAYLOAD_SIZE 32768

static const size_t PAYLOAD_SIZES[N_SIZES] = {
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768
};

static const char *SIZE_NAMES[N_SIZES] = {
    "256B", "384B", "512B", "768B", "1KB", "1.5KB", "2KB", "3KB", 
    "4KB", "6KB", "8KB", "12KB", "16KB", "24KB", "32KB"
};

/* Large static buffer for all payloads */
static unsigned char payload_buffer[32768];

/* Pointers to each payload section [part] */
static unsigned char *test_payloads[N_SIZES];

/* ─────────────────────────────────────────────────────────────────────────── */
/* Global state */
/* ─────────────────────────────────────────────────────────────────────────── */

static WOLFSSL_CTX *g_server_ctx = NULL;
static volatile int g_shutdown = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Signal handling */
/* ─────────────────────────────────────────────────────────────────────────── */

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_shutdown = 1;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Initialize test payloads */
/* ─────────────────────────────────────────────────────────────────────────── */

static void init_test_payloads(void) {
    /* Fill large buffer with recognizable pattern [recognizable design] for validation [checking] */
    for (int i = 0; i < MAX_PAYLOAD_SIZE; i++)
        payload_buffer[i] = (unsigned char)(i % 256);
    
    /* Set up pointers [references] to each payload size section [part] */
    size_t offset = 0;
    for (int i = 0; i < N_SIZES; i++) {
        test_payloads[i] = payload_buffer + offset;
        offset += PAYLOAD_SIZES[i];
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Socket utilities */
/* ─────────────────────────────────────────────────────────────────────────── */

static void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static void set_blocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* TLS setup */
/* ─────────────────────────────────────────────────────────────────────────── */

static int setup_tls_context(void) {
    /* Initialize wolfSSL */
    fprintf(stderr, "[SERVER] Initializing wolfSSL...\n");
    if (wolfSSL_library_init() != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] wolfSSL_library_init() failed\n");
        return 1;
    }
    
    /* Create context */
    fprintf(stderr, "[SERVER] Creating TLS server context...\n");
    g_server_ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!g_server_ctx) {
        fprintf(stderr, "[ERROR] wolfSSL_CTX_new() failed\n");
        return 1;
    }
    
    /* Load server certificate and key (same as MB-1) */
    fprintf(stderr, "[SERVER] Loading certificate from ./wolfssl_certs/server-cert.der\n");
    if (wolfSSL_CTX_use_certificate_file(g_server_ctx, 
            "./wolfssl_certs/server-cert.der", SSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server certificate\n");
        fprintf(stderr, "[ERROR] Make sure: scp -r wolfssl/certs pi@<PI_IP>:~/wolfssl_certs\n");
        return 1;
    }
    
    fprintf(stderr, "[SERVER] Loading key from ./wolfssl_certs/server-key.der\n");
    if (wolfSSL_CTX_use_PrivateKey_file(g_server_ctx,
            "./wolfssl_certs/server-key.der", SSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server key\n");
        return 1;
    }
    
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Client connection handler */
/* ─────────────────────────────────────────────────────────────────────────── */

static int handle_client(int client_sock, struct sockaddr_in *client_addr) {
    WOLFSSL *ssl;
    int ret = 0;
    
    fprintf(stderr, "[SERVER] Handling client from %s:%d\n",
            inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
    
    /* Create SSL object for this connection */
    ssl = wolfSSL_new(g_server_ctx);
    if (!ssl) {
        fprintf(stderr, "[ERROR] wolfSSL_new() failed\n");
        return 1;
    }
    
    /* Set socket */
    if (wolfSSL_set_fd(ssl, client_sock) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] wolfSSL_set_fd() failed\n");
        wolfSSL_free(ssl);
        return 1;
    }
    
    /* TLS handshake */
    fprintf(stderr, "[SERVER] Performing TLS handshake...\n");
    if (wolfSSL_accept(ssl) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] TLS handshake failed\n");
        wolfSSL_free(ssl);
        return 1;
    }
    
    fprintf(stderr, "[SERVER] ✓ TLS connection established\n");
    
    /* Send test protocol: sizes and payloads */
    fprintf(stderr, "[SERVER] Starting payload transmission...\n");
    
    /* Send magic header to client */
    unsigned char header[4] = {'M', 'B', '2', '0'};
    if (wolfSSL_write(ssl, header, 4) != 4) {
        fprintf(stderr, "[ERROR] Failed to send header\n");
        wolfSSL_free(ssl);
        return 1;
    }
    
    /* For each payload size */
    for (int size_idx = 0; size_idx < N_SIZES; size_idx++) {
        size_t payload_size = PAYLOAD_SIZES[size_idx];
        fprintf(stderr, "[SERVER]   Sending %s payloads (%zu bytes each)...\n",
                SIZE_NAMES[size_idx], payload_size);
        
        /* Send 100 payloads of this size (client will repeat internally) */
        for (int i = 0; i < 100; i++) {
            int sent = wolfSSL_write(ssl, test_payloads[size_idx], payload_size);
            if (sent != (int)payload_size) {
                fprintf(stderr, "[ERROR] Failed to send payload %d of size %s\n", i, SIZE_NAMES[size_idx]);
                wolfSSL_free(ssl);
                return 1;
            }
        }
    }
    
    fprintf(stderr, "[SERVER] ✓ All payloads sent\n");
    
    /* Keep connection open briefly for client to process */
    sleep(2);
    
    /* Cleanup */
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_sock);
    
    fprintf(stderr, "[SERVER] Client connection closed\n");
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main server loop */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    int reuse = 1;
    
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  MB-2 SERVER - read_peek() overhead measurement           ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n\n");
    
    /* Initialize test data */
    init_test_payloads();
    
    /* Setup TLS */
    if (setup_tls_context() != 0) {
        return 1;
    }
    
    /* Create server socket */
    fprintf(stderr, "[SERVER] Creating socket...\n");
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    
    /* Allow reuse of address */
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(server_sock);
        return 1;
    }
    
    /* Bind */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MB2_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    fprintf(stderr, "[SERVER] Binding to port %d...\n", MB2_PORT);
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    
    /* Listen */
    if (listen(server_sock, BACKLOG) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }
    
    fprintf(stderr, "[SERVER] Listening on 0.0.0.0:%d\n", MB2_PORT);
    fprintf(stderr, "[SERVER] Waiting for client connection...\n\n");
    
    /* Setup signal handler */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    /* Accept connections */
    while (!g_shutdown) {
        client_addr_len = sizeof(client_addr);
        
        /* Accept with timeout */
        fd_set readfds;
        struct timeval tv;
        int select_ret;
        
        FD_ZERO(&readfds);
        FD_SET(server_sock, &readfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        
        select_ret = select(server_sock + 1, &readfds, NULL, NULL, &tv);
        
        if (select_ret < 0) {
            perror("select");
            break;
        }
        
        if (select_ret == 0) {
            /* Timeout, check for shutdown */
            continue;
        }
        
        /* Accept connection */
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }
        
        /* Handle client */
        handle_client(client_sock, &client_addr);
    }
    
    /* Cleanup */
    fprintf(stderr, "[SERVER] Cleaning up...\n");
    wolfSSL_CTX_free(g_server_ctx);
    close(server_sock);
    
    fprintf(stderr, "[SERVER] Server stopped.\n");
    return 0;
}
