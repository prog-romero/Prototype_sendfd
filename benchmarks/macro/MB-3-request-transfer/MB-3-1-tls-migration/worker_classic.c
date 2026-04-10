/*
 * worker_classic.c — PATH B: Accept fresh TLS 1.3 handshake
 *
 * This worker:
 * 1. Listens on TCP port for incoming connections (from gateway)
 * 2. Measures time to complete TLS 1.3 handshake
 * 3. Reads HTTP request
 * 4. Sends HTTP response
 * 5. Records handshake timing for each iteration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>

#include "config.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/* GLOBAL STATE */
/* ─────────────────────────────────────────────────────────────────────────── */

static volatile int keep_running = 1;
WOLFSSL_CTX *ctx = NULL;
static FILE *timing_file = NULL;

/* ─────────────────────────────────────────────────────────────────────────── */
/* UTILITIES */
/* ─────────────────────────────────────────────────────────────────────────── */

void signal_handler(int sig) {
    fprintf(stderr, "[worker_classic] Received signal %d, shutting down\n", sig);
    keep_running = 0;
}

uint64_t timespec_to_us(struct timespec t) {
    return (t.tv_sec * 1000000UL) + (t.tv_nsec / 1000UL);
}

uint32_t timespec_to_ns_frac(struct timespec t) {
    return t.tv_nsec % 1000;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* CLIENT HANDLER */
/* ─────────────────────────────────────────────────────────────────────────── */

void handle_client(int client_fd, FILE *csv_out) {
    WOLFSSL *ssl = NULL;
    struct timespec start, stop;
    
    /* Create SSL session */
    ssl = wolfSSL_new(ctx);
    if (!ssl) {
        fprintf(stderr, "[worker_classic] wolfSSL_new() failed\n");
        goto cleanup;
    }
    
    /* Attach socket to SSL session */
    wolfSSL_set_fd(ssl, client_fd);
    
    /* TIMER START: Right before accept (which does TLS handshake) */
    clock_gettime(CLOCK_SOURCE, &start);
    
    /* ACCEPT: This performs the complete TLS 1.3 handshake */
    int ret = wolfSSL_accept(ssl);
    
    /* TIMER STOP: Right after handshake completes */
    clock_gettime(CLOCK_SOURCE, &stop);
    
    if (ret != SSL_SUCCESS) {
        fprintf(stderr, "[worker_classic] wolfSSL_accept() failed: %d\n", ret);
        goto cleanup;
    }
    
    /* Calculate handshake timing */
    uint64_t handshake_us = timespec_to_us(stop) - timespec_to_us(start);
    
    fprintf(stderr, "[worker_classic] TLS handshake completed in %llu µs\n", 
            (unsigned long long)handshake_us);
    
    /* Read HTTP request */
    char request[REQUEST_SIZE];
    int n = wolfSSL_read(ssl, (void*)request, sizeof(request) - 1);
    if (n > 0) {
        request[n] = '\0';
        fprintf(stderr, "[worker_classic] Received request: %s\n", request);
    } else {
        fprintf(stderr, "[worker_classic] wolfSSL_read() failed\n");
    }
    
    /* Send HTTP response */
    const char *response = HTTP_RESPONSE;
    wolfSSL_write(ssl, (void*)response, strlen(response));
    
    /* Record timing to CSV (microseconds only, like gateway and worker_migration) */
    fprintf(csv_out, "%llu\n", (unsigned long long)handshake_us);
    fflush(csv_out);
    
cleanup:
    if (ssl) {
        /* Use quiet shutdown to avoid sending close_notify alert. */
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    close(client_fd);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* INITIALIZATION & MAIN */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Initialize wolfSSL */
    wolfSSL_library_init();
    
    /* Create SSL context */
    ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[worker_classic] wolfSSL_CTX_new() failed\n");
        return 1;
    }
    
    /* Load server certificate */
    if (wolfSSL_CTX_use_certificate_file(ctx, SERVER_CERT, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[worker_classic] Failed to load certificate: %s\n", SERVER_CERT);
        return 1;
    }
    
    /* Load server private key */
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, SERVER_KEY, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[worker_classic] Failed to load private key: %s\n", SERVER_KEY);
        return 1;
    }
    
    /* Create results directory if needed */
    system("mkdir -p " RESULTS_DIR);
    
    /* Open results file for appending timing data */
    timing_file = fopen(RESULTS_CSV_HANDSHAKE, "a");
    if (!timing_file) {
        fprintf(stderr, "[worker_classic] Failed to open %s\n", RESULTS_CSV_HANDSHAKE);
        return 1;
    }
    
    /* Create TCP socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[worker_classic] socket()");
        return 1;
    }
    
    /* Allow socket reuse */
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[worker_classic] setsockopt(SO_REUSEADDR)");
        return 1;
    }
    
    /* Bind to TCP port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WORKER_CLASSIC_PORT);
    addr.sin_addr.s_addr = inet_addr(LOCALHOST);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[worker_classic] bind()");
        return 1;
    }
    
    if (listen(listen_fd, 100) < 0) {
        perror("[worker_classic] listen()");
        return 1;
    }
    
    fprintf(stderr, "[worker_classic] Listening on %s:%d\n", LOCALHOST, WORKER_CLASSIC_PORT);
    fprintf(stderr, "[worker_classic] Ready for connections...\n");
    
    /* Accept and handle connections */
    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (keep_running) {
                perror("[worker_classic] accept()");
            }
            break;
        }
        
        fprintf(stderr, "[worker_classic] Accepted connection from %s:%d\n",
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        handle_client(client_fd, timing_file);
    }
    
    /* Cleanup */
    fprintf(stderr, "[worker_classic] Shutting down gracefully\n");
    if (timing_file) {
        fclose(timing_file);
    }
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    close(listen_fd);
    
    return 0;
}
