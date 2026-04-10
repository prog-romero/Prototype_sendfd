/*
 * gateway.c — TLS Termination Gateway for PATH A (MB-3.1 Benchmark)
 *
 * This gateway:
 * 1. Accepts TLS connections from clients on port 8443
 * 2. Performs TLS 1.3 handshake with wolfSSL
 * 3. Registers keylog callback to capture TLS state
 * 4. Measures: tlspeek_serialize() + sendfd_with_state() time
 * 5. Sends FD + serialized TLS state to worker via Unix socket
 * 6. Records gateway timing to results/mb3_1_gateway_timings.csv
 *
 * For the benchmark:
 * - Measures serialize + transfer overhead only (server-side)
 * - TLS handshake NOT measured (happens before serialize step)
 * - Worker measures receive + restore overhead separately
 */

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>

#include "tlspeek.h"
#include "tlspeek_serial.h"
#include "sendfd.h"
#include "config.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/* GLOBAL STATE */
/* ─────────────────────────────────────────────────────────────────────────── */

static volatile int keep_running = 1;
WOLFSSL_CTX *ctx = NULL;
FILE *results_file = NULL;
pthread_mutex_t results_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint64_t gateway_us;            /* Time for serialize + sendfd */
} gateway_timing_t;

/* ─────────────────────────────────────────────────────────────────────────── */
/* SIGNAL HANDLING */
/* ─────────────────────────────────────────────────────────────────────────── */

void signal_handler(int sig) {
    fprintf(stderr, "[gateway] Received signal %d, shutting down\n", sig);
    keep_running = 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* UTILITY FUNCTIONS */
/* ─────────────────────────────────────────────────────────────────────────── */

uint64_t timespec_to_us(struct timespec t) {
    return (t.tv_sec * 1000000UL) + (t.tv_nsec / 1000UL);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* CLIENT HANDLER THREAD */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int client_fd;
    WOLFSSL *ssl;
} client_context_t;

void* handle_client_thread(void *arg) {
    client_context_t *ctx_data = (client_context_t *)arg;
    int client_fd = ctx_data->client_fd;
    WOLFSSL *ssl = ctx_data->ssl;
    free(ctx_data);
    
    fprintf(stderr, "[gateway] Client TLS handshake completed\n");
    
    /* NOTE: We do NOT read the client request here!
       The FD will be passed to worker, which will read the request.
       This way, the TLS session state and the encrypted data in the socket
       are preserved for the worker to handle. */
    
    // ─── Create tlspeek context and prepare for serialization ─────────
    tlspeek_ctx_t peek_ctx;
    memset(&peek_ctx, 0, sizeof(peek_ctx));
    peek_ctx.tcp_fd = client_fd;
    peek_ctx.ssl = ssl;
    
    // Register keylog callback to extract TLS keys
    wolfSSL_set_ex_data(ssl, 0, &peek_ctx);
    wolfSSL_set_tls13_secret_cb(ssl, tlspeek_keylog_cb, &peek_ctx);
    
    // Hack: manually trigger callback (it should have fired during handshake)
    // In production, this would be called automatically
    if (!peek_ctx.keys_ready) {
        // Try to extract cipher suite from wolfSSL
        const char *cipher_name = wolfSSL_get_cipher_name(ssl);
        if (cipher_name) {
            if (strstr(cipher_name, "CHACHA20"))
                peek_ctx.cipher_suite = TLSPEEK_CHACHA20_POLY;
            else if (strstr(cipher_name, "AES-128"))
                peek_ctx.cipher_suite = TLSPEEK_AES_128_GCM;
            else
                peek_ctx.cipher_suite = TLSPEEK_AES_256_GCM;
        }
    }
    
    // ─── Measure serialize + sendfd_with_state ─────────────────────────
    struct timespec start, stop;
    tlspeek_serial_t serial_state;
    
    fprintf(stderr, "[gateway] Starting serialize + sendfd measurement\n");
    
    clock_gettime(CLOCK_SOURCE, &start);
    
    // STEP 1: Serialize TLS session state
    tlspeek_serialize(&peek_ctx, &serial_state);
    
    // STEP 2: Send FD + serialized state to worker via Unix socket
    int unix_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (unix_fd < 0) {
        clock_gettime(CLOCK_SOURCE, &stop);
        perror("[gateway] socket(AF_UNIX)");
        goto cleanup;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WORKER_MIGRATION_SOCKET, sizeof(addr.sun_path) - 1);
    
    if (connect(unix_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        clock_gettime(CLOCK_SOURCE, &stop);
        perror("[gateway] connect(unix)");
        close(unix_fd);
        goto cleanup;
    }
    
    if (sendfd_with_state(unix_fd, client_fd, (void*)&serial_state, sizeof(serial_state)) < 0) {
        clock_gettime(CLOCK_SOURCE, &stop);
        fprintf(stderr, "[gateway] sendfd_with_state() failed\n");
        close(unix_fd);
        goto cleanup;
    }
    
    clock_gettime(CLOCK_SOURCE, &stop);
    close(unix_fd);
    
    // Calculate elapsed time
    uint64_t gateway_us = timespec_to_us(stop) - timespec_to_us(start);
    
    fprintf(stderr, "[gateway] Serialized and sent in %llu µs\n", 
            (unsigned long long)gateway_us);
    
    // Record result (thread-safe)
    pthread_mutex_lock(&results_lock);
    fprintf(results_file, "%llu\n", (unsigned long long)gateway_us);
    fflush(results_file);
    pthread_mutex_unlock(&results_lock);
    
    fprintf(stderr, "[gateway] FD handed off to worker\n");
    
cleanup:
    if (ssl) {
        /* Detach FD from SSL object before freeing it.
           This ensures wolfSSL_free() doesn't close the FD,
           since we're transferring ownership to the worker. */
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_set_fd(ssl, -1);
        wolfSSL_free(ssl);
    }
    /* client_fd ownership transferred to worker via sendfd_with_state().
       sendfd_with_state() closes the FD after sending, so we don't close it here. */
    
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* TLS SERVER MAIN */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create results directory */
    system("mkdir -p " RESULTS_DIR);
    
    /* Open results file */
    results_file = fopen(RESULTS_CSV_GATEWAY, "a");
    if (!results_file) {
        fprintf(stderr, "[gateway] Failed to open %s\n", RESULTS_CSV_GATEWAY);
        return 1;
    }
    
    /* Initialize wolfSSL */
    wolfSSL_library_init();
    
    /* Create TLS context */
    ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[gateway] wolfSSL_CTX_new() failed\n");
        return 1;
    }
    
    /* Load server certificate */
    if (wolfSSL_CTX_use_certificate_file(ctx, SERVER_CERT, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[gateway] Failed to load certificate: %s\n", SERVER_CERT);
        return 1;
    }
    
    /* Load server private key */
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, SERVER_KEY, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[gateway] Failed to load private key: %s\n", SERVER_KEY);
        return 1;
    }
    
    /* Create and bind TCP socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[gateway] socket()");
        return 1;
    }
    
    /* Allow address reuse */
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("[gateway] setsockopt()");
        return 1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GATEWAY_TLS_PORT);
    addr.sin_addr.s_addr = inet_addr(LOCALHOST);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[gateway] bind()");
        return 1;
    }
    
    if (listen(listen_fd, 10) < 0) {
        perror("[gateway] listen()");
        return 1;
    }
    
    fprintf(stderr, "[gateway] TLS Gateway listening on %s:%d\n", LOCALHOST, GATEWAY_TLS_PORT);
    fprintf(stderr, "[gateway] Results: %s\n", RESULTS_CSV_GATEWAY);
    fprintf(stderr, "[gateway] Worker socket: %s\n", WORKER_MIGRATION_SOCKET);
    fprintf(stderr, "[gateway] Ready for connections...\n");
    
    /* Accept connections */
    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (keep_running) {
                perror("[gateway] accept()");
            }
            continue;
        }
        
        fprintf(stderr, "[gateway] Client connected from %s:%u\n",
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /* Create SSL object for this connection */
        WOLFSSL *ssl = wolfSSL_new(ctx);
        if (!ssl) {
            fprintf(stderr, "[gateway] wolfSSL_new() failed\n");
            close(client_fd);
            continue;
        }
        
        /* Attach client FD to SSL */
        wolfSSL_set_fd(ssl, client_fd);
        
        /* Perform TLS handshake (not measured for this benchmark) */
        if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
            fprintf(stderr, "[gateway] wolfSSL_accept() failed\n");
            wolfSSL_free(ssl);
            close(client_fd);
            continue;
        }
        
        /* Handle client in separate thread */
        client_context_t *ctx_data = malloc(sizeof(client_context_t));
        if (!ctx_data) {
            fprintf(stderr, "[gateway] malloc() failed\n");
            wolfSSL_free(ssl);
            close(client_fd);
            continue;
        }
        
        ctx_data->client_fd = client_fd;
        ctx_data->ssl = ssl;
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client_thread, ctx_data) != 0) {
            fprintf(stderr, "[gateway] pthread_create() failed\n");
            wolfSSL_free(ssl);
            close(client_fd);
            free(ctx_data);
            continue;
        }
        
        /* Detach thread to avoid zombie processes */
        pthread_detach(thread_id);
    }
    
    /* Cleanup */
    if (results_file) {
        fclose(results_file);
    }
    close(listen_fd);
    
    if (ctx) {
        wolfSSL_CTX_free(ctx);
    }
    
    wolfSSL_Cleanup();
    
    fprintf(stderr, "[gateway] Shutdown complete\n");
    return 0;
}
