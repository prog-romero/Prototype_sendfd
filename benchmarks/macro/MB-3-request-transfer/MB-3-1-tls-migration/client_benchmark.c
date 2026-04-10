/*
 * client_benchmark.c — MB-3.1 Client: Test PATH A or PATH B
 *
 * Usage:
 *   ./client_benchmark --path A    # Test TLS Migration (gateway → worker)
 *   ./client_benchmark --path B    # Test Fresh Handshake (direct worker)
 *
 * For each iteration:
 *   PATH A: Connect to gateway TLS on 8443, measures serialize + transfer + restore
 *   PATH B: Connect to worker_classic on 9001, measures fresh TLS 1.3 handshake
 *
 * Both paths do server-side timing (not client round-trip).
 * Results written to separate CSV files that are merged post-experiment.
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

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>

#include "config.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/* COMMAND LINE PARSING */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    PATH_NONE = 0,
    PATH_A = 1,     /* TLS Migration */
    PATH_B = 2      /* Fresh Handshake */
} benchmark_path_t;

benchmark_path_t parse_path_arg(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s --path [A|B]\n", argv[0]);
        fprintf(stderr, "  --path A   Test TLS Migration (gateway → worker)\n");
        fprintf(stderr, "  --path B   Test Fresh Handshake (direct worker)\n");
        exit(1);
    }
    
    if (strcmp(argv[1], "--path") != 0) {
        fprintf(stderr, "Error: First argument must be '--path'\n");
        exit(1);
    }
    
    if (argc < 3) {
        fprintf(stderr, "Error: --path requires an argument [A|B]\n");
        exit(1);
    }
    
    if (argv[2][0] == 'A' || argv[2][0] == 'a') {
        return PATH_A;
    } else if (argv[2][0] == 'B' || argv[2][0] == 'b') {
        return PATH_B;
    } else {
        fprintf(stderr, "Error: --path argument must be 'A' or 'B'\n");
        exit(1);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* TCP CONNECTION */
/* ─────────────────────────────────────────────────────────────────────────── */

int connect_tcp_socket(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(AF_INET)");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect(tcp)");
        close(fd);
        return -1;
    }
    
    return fd;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* PATH A: TLS MIGRATION TEST */
/* ─────────────────────────────────────────────────────────────────────────── */

void test_migration(WOLFSSL_CTX *client_ctx, int iteration) {
    /*
     * PATH A Flow (TLS Migration):
     * 1. Client connects TLS to gateway on port 8443
     * 2. Sends HTTP request
     * 3. Gateway measures: serialize + sendfd_with_state time
     * 4. Gateway sends FD to worker
     * 5. Worker measures: recvfd_with_state + restore time
     * 6. Both write to separate CSV files (merged later)
     *
     * Note: Timing is measured server-side, not client round-trip
     */
    
    int worker_fd = connect_tcp_socket(LOCALHOST, GATEWAY_TLS_PORT);
    if (worker_fd < 0) {
        fprintf(stderr, "[client-PATH-A] Iteration %d: Failed to connect to gateway TLS\n", iteration);
        return;
    }
    
    /* Create TLS connection */
    WOLFSSL *ssl = wolfSSL_new(client_ctx);
    if (!ssl) {
        fprintf(stderr, "[client-PATH-A] Iteration %d: wolfSSL_new() failed\n", iteration);
        close(worker_fd);
        return;
    }
    
    wolfSSL_set_fd(ssl, worker_fd);
    
    if (wolfSSL_connect(ssl) != SSL_SUCCESS) {
        fprintf(stderr, "[client-PATH-A] Iteration %d: wolfSSL_connect() failed\n", iteration);
        wolfSSL_free(ssl);
        close(worker_fd);
        return;
    }
    
    /* Send HTTP request */
    const char *request = HTTP_REQUEST;
    wolfSSL_write(ssl, (void*)request, strlen(request));
    
    /* Read response */
    char response[RESPONSE_SIZE];
    int n = wolfSSL_read(ssl, (void*)response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        fprintf(stderr, "[client-PATH-A] Iteration %d: Received %d bytes\n", iteration, n);
    } else {
        fprintf(stderr, "[client-PATH-A] Iteration %d: wolfSSL_read() failed\n", iteration);
    }
    
    /* Cleanup */
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(worker_fd);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* PATH B: FRESH HANDSHAKE TEST */
/* ─────────────────────────────────────────────────────────────────────────── */

void test_handshake(WOLFSSL_CTX *client_ctx, int iteration) {
    /*
     * PATH B Flow (Fresh TLS 1.3 Handshake):
     * 1. Client connects TLS to worker_classic on port 9001
     * 2. Worker performs full TLS 1.3 handshake
     * 3. Worker measures: wolfSSL_accept() time
     * 4. Client sends HTTP request
     * 5. Worker responds
     *
     * Note: Timing is measured server-side in wolfSSL_accept()
     */
    
    int worker_fd = connect_tcp_socket(LOCALHOST, WORKER_CLASSIC_PORT);
    if (worker_fd < 0) {
        fprintf(stderr, "[client-PATH-B] Iteration %d: Failed to connect to worker_classic\n", iteration);
        return;
    }
    
    /* Create TLS connection */
    WOLFSSL *ssl = wolfSSL_new(client_ctx);
    if (!ssl) {
        fprintf(stderr, "[client-PATH-B] Iteration %d: wolfSSL_new() failed\n", iteration);
        close(worker_fd);
        return;
    }
    
    wolfSSL_set_fd(ssl, worker_fd);
    
    /* Perform TLS 1.3 handshake */
    if (wolfSSL_connect(ssl) != SSL_SUCCESS) {
        fprintf(stderr, "[client-PATH-B] Iteration %d: wolfSSL_connect() failed\n", iteration);
        wolfSSL_free(ssl);
        close(worker_fd);
        return;
    }
    
    /* Send HTTP request */
    const char *request = HTTP_REQUEST;
    wolfSSL_write(ssl, (void*)request, strlen(request));
    
    /* Read response */
    char response[RESPONSE_SIZE];
    int n = wolfSSL_read(ssl, (void*)response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        fprintf(stderr, "[client-PATH-B] Iteration %d: Received %d bytes\n", iteration, n);
    } else {
        fprintf(stderr, "[client-PATH-B] Iteration %d: wolfSSL_read() failed\n", iteration);
    }
    
    /* Cleanup */
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(worker_fd);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MAIN BENCHMARK LOOP */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    benchmark_path_t path = parse_path_arg(argc, argv);
    
    if (path == PATH_A) {
        printf("[client] Starting MB-3.1 benchmark: PATH A (TLS Migration)\n");
    } else {
        printf("[client] Starting MB-3.1 benchmark: PATH B (Fresh Handshake)\n");
    }
    
    printf("[client] ITERATIONS_WARMUP=%d, ITERATIONS_TOTAL=%d\n",
           ITERATIONS_WARMUP, ITERATIONS_TOTAL);
    
    /* Initialize wolfSSL */
    wolfSSL_library_init();
    
    /* Create client context */
    WOLFSSL_CTX *client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[client] wolfSSL_CTX_new() failed\n");
        return 1;
    }
    
    /* Skip certificate verification for this benchmark */
    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    
    /* ─────────────────────────────────────────────────────────────────────── */
    /* WARMUP PHASE */
    /* ─────────────────────────────────────────────────────────────────────── */
    
    printf("[client] WARMUP: Running %d iterations (discarded)...\n", ITERATIONS_WARMUP);
    
    for (int i = 0; i < ITERATIONS_WARMUP; i++) {
        if (i % 10 == 0) printf("  [%d/%d]\n", i, ITERATIONS_WARMUP);
        
        if (path == PATH_A) {
            test_migration(client_ctx, i);
        } else {
            test_handshake(client_ctx, i);
        }
        
        usleep(10000);  /* 10ms delay between iterations */
    }
    
    printf("[client] Warmup complete\n\n");
    
    /* ─────────────────────────────────────────────────────────────────────── */
    /* MEASUREMENT PHASE */
    /* ─────────────────────────────────────────────────────────────────────── */
    
    printf("[client] MEASUREMENT: Running %d iterations...\n", ITERATIONS_TOTAL);
    
    for (int i = 0; i < ITERATIONS_TOTAL; i++) {
        if (i % 100 == 0) printf("  [%d/%d]\n", i, ITERATIONS_TOTAL);
        
        if (path == PATH_A) {
            test_migration(client_ctx, i + ITERATIONS_WARMUP);
        } else {
            test_handshake(client_ctx, i + ITERATIONS_WARMUP);
        }
        
        usleep(10000);  /* 10ms delay */
    }
    
    printf("[client] Measurement complete\n");
    
    if (path == PATH_A) {
        printf("[client] Gateway timings: %s\n", RESULTS_CSV_GATEWAY);
        printf("[client] Worker timings:  %s\n", RESULTS_CSV_WORKER);
    } else {
        printf("[client] Handshake timings: %s\n", RESULTS_CSV_HANDSHAKE);
    }
    
    /* Cleanup */
    wolfSSL_CTX_free(client_ctx);
    wolfSSL_Cleanup();
    
    printf("[client] Benchmark finished successfully\n");
    
    return 0;
}
