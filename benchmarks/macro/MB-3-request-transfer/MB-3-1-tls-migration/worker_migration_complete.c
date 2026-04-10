/*
 * worker_migration_complete.c — PATH A WORKER: Receive FD + State, Measure Restore
 *
 * This worker:
 * 1. Listens on Unix domain socket for fd + tlspeek_serial_t
 * 2. Measures time from recvfd_with_state() to tlspeek_restore() completion
 * 3. Serves HTTP request on the migrated TLS connection
 * 4. Records timing to results/mb3_1_worker_timings.csv
 *
 * Measurement: Tworker_restore = recvfd_with_state() + tlspeek_restore() time
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

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

typedef struct {
    uint64_t worker_us;             /* Time spent in recvfd + restore */
    uint32_t worker_ns;             /* Nanosecond precision */
} worker_timing_t;

static FILE *timing_file = NULL;

/* ─────────────────────────────────────────────────────────────────────────── */
/* UTILITIES */
/* ─────────────────────────────────────────────────────────────────────────── */

void signal_handler(int sig) {
    fprintf(stderr, "[worker_migration] Received signal %d, shutting down\n", sig);
    keep_running = 0;
}

uint64_t timespec_to_us(struct timespec t) {
    return (t.tv_sec * 1000000UL) + (t.tv_nsec / 1000UL);
}

uint32_t timespec_to_ns_frac(struct timespec t) {
    return t.tv_nsec % 1000U;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* CONNECTION HANDLER */
/* ─────────────────────────────────────────────────────────────────────────── */

void handle_client(int client_fd, FILE *csv_out) {
    WOLFSSL *ssl = NULL;
    int inherited_fd = -1;
    tlspeek_serial_t serial_state;
    struct timespec start, stop;
    
    fprintf(stderr, "[worker_migration] Handling new client\n");
    
    /* TIMER START: Right before recvfd_with_state */
    clock_gettime(CLOCK_SOURCE, &start);
    
    /* RECEIVE FD + SERIALIZED TLS STATE from gateway */
    if (recvfd_with_state(client_fd, &inherited_fd, (void*)&serial_state, sizeof(serial_state)) < 0) {
        clock_gettime(CLOCK_SOURCE, &stop);
        fprintf(stderr, "[worker_migration] recvfd_with_state() failed\n");
        goto cleanup;
    }
    
    /* Create SSL object and attach inherited fd */
    ssl = wolfSSL_new(ctx);
    if (!ssl) {
        clock_gettime(CLOCK_SOURCE, &stop);
        fprintf(stderr, "[worker_migration] wolfSSL_new() failed\n");
        goto cleanup;
    }
    
    wolfSSL_set_fd(ssl, inherited_fd);
    
    /* RESTORE TLS STATE from serialized blob */
    if (tlspeek_restore(ssl, &serial_state) < 0) {
        clock_gettime(CLOCK_SOURCE, &stop);
        fprintf(stderr, "[worker_migration] tlspeek_restore() failed\n");
        goto cleanup;
    }
    
    /* TIMER STOP: Right after restore completes */
    clock_gettime(CLOCK_SOURCE, &stop);
    
    /* Calculate timing */
    uint64_t worker_us = timespec_to_us(stop) - timespec_to_us(start);
    
    fprintf(stderr, "[worker_migration] Restored and received in %llu µs\n", 
            (unsigned long long)worker_us);
    
    /* Now read HTTP request from inherited fd (which has restored TLS state) */
    char request[REQUEST_SIZE];
    int n = wolfSSL_read(ssl, (void*)request, sizeof(request) - 1);
    if (n > 0) {
        request[n] = '\0';
        fprintf(stderr, "[worker_migration] Received request: %s\n", request);
    } else {
        fprintf(stderr, "[worker_migration] wolfSSL_read() failed\n");
    }
    
    /* Send HTTP response */
    const char *response = HTTP_RESPONSE;
    wolfSSL_write(ssl, (void*)response, strlen(response));
    
    /* Record timing to CSV (worker only) */
    fprintf(csv_out, "%llu\n", (unsigned long long)worker_us);
    fflush(csv_out);
    
cleanup:
    if (ssl) {
        /* Use quiet shutdown to avoid sending close_notify alert.
           This matches the libtlspeek pattern since the connection
           is being processed quickly. */
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    if (inherited_fd >= 0) {
        close(inherited_fd);
    }
    close(client_fd);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* INITIALIZATION & MAIN */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Load certificate and key */
    ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[worker_migration] wolfSSL_CTX_new() failed\n");
        return 1;
    }
    
    if (wolfSSL_CTX_use_certificate_file(ctx, SERVER_CERT, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[worker_migration] Failed to load certificate: %s\n", SERVER_CERT);
        return 1;
    }
    
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, SERVER_KEY, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[worker_migration] Failed to load private key: %s\n", SERVER_KEY);
        return 1;
    }
    
    /* Create results directory if needed */
    system("mkdir -p " RESULTS_DIR);
    
    /* Open CSV file for results */
    timing_file = fopen(RESULTS_CSV_WORKER, "a");
    if (!timing_file) {
        fprintf(stderr, "[worker_migration] Failed to open %s\n", RESULTS_CSV_WORKER);
        return 1;
    }
    
    /* Create and bind Unix domain socket */
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (listen_fd < 0) {
        perror("[worker_migration] socket()");
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WORKER_MIGRATION_SOCKET, sizeof(addr.sun_path) - 1);
    
    /* Remove old socket file if exists */
    unlink(WORKER_MIGRATION_SOCKET);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[worker_migration] bind()");
        return 1;
    }
    
    if (listen(listen_fd, 100) < 0) {
        perror("[worker_migration] listen()");
        return 1;
    }
    
    fprintf(stderr, "[worker_migration] Listening on %s\n", WORKER_MIGRATION_SOCKET);
    fprintf(stderr, "[worker_migration] Ready for connections...\n");
    
    /* Accept and handle connections */
    while (keep_running) {
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (keep_running) {
                perror("[worker_migration] accept()");
            }
            break;
        }
        
        handle_client(client_fd, timing_file);
    }
    
    /* Cleanup */
    if (timing_file) {
        fclose(timing_file);
    }
    close(listen_fd);
    unlink(WORKER_MIGRATION_SOCKET);
    
    if (ctx) {
        wolfSSL_CTX_free(ctx);
    }
    
    fprintf(stderr, "[worker_migration] Shutdown complete\n");
    return 0;
}
