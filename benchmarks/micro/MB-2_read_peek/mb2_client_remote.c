/*
 * MB-2 Client - Measure tls_read_peek() vs wolfSSL_read() overhead
 * Runs on local machine
 * 
 * Measures overhead of:
 *   Config A: wolfSSL_read() only
 *   Config B: tls_read_peek() + wolfSSL_read()
 * 
 * For each payload size: 256B to 32KB (15 sizes total)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>

#include <wolfssl/ssl.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration */
/* ─────────────────────────────────────────────────────────────────────────── */

#define MB2_PORT 19446
#define TIMEOUT_SEC 30
#define N_SIZES 15
#define ITERATIONS 1000

static const size_t PAYLOAD_SIZES[N_SIZES] = {
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768
};

static const char *SIZE_NAMES[N_SIZES] = {
    "256B", "384B", "512B", "768B", "1KB", "1.5KB", "2KB", "3KB", 
    "4KB", "6KB", "8KB", "12KB", "16KB", "24KB", "32KB"
};

/* ─────────────────────────────────────────────────────────────────────────── */
/* Timing utilities */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    struct timespec start;
    struct timespec end;
} benchmark_timer_t;

static inline void timer_start(benchmark_timer_t *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->start);
}

static inline void timer_stop(benchmark_timer_t *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->end);
}

static inline double timer_us(benchmark_timer_t *t) {
    long sec_diff = t->end.tv_sec - t->start.tv_sec;
    long nsec_diff = t->end.tv_nsec - t->start.tv_nsec;
    double us = (sec_diff * 1e6) + (nsec_diff / 1e3);
    return us;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Result structure */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    size_t payload_size;
    char config;                    /* 'A' or 'B' */
    int iterations;
    double total_time_us;
    double avg_time_per_iter;
    double stddev;
    int failed_count;
    double *iteration_times;
} benchmark_result_t;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Statistics */
/* ─────────────────────────────────────────────────────────────────────────── */

static double calculate_stddev(double *values, int count) {
    if (count < 2) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    double mean = sum / count;
    
    double variance = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = values[i] - mean;
        variance += diff * diff;
    }
    variance /= count;
    
    return sqrt(variance);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Config A: Normal wolfSSL_read() only */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_read_only(const char *server_ip, size_t payload_size,
                               int iterations, benchmark_result_t *result) {
    int client_sock, failed = 0;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *client_ctx;
    WOLFSSL *ssl;
    benchmark_timer_t timer;
    unsigned char buffer[8192];  /* Read buffer */
    int bytes_read;
    
    fprintf(stderr, "[MB-2-A] wolfSSL_read() only: payload=%zu bytes, iterations=%d\n",
            payload_size, iterations);
    
    /* Create context */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client context\n");
        return 1;
    }
    
    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    
    /* Connect to server */
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        perror("socket");
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MB2_PORT);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    
    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    /* Create SSL connection */
    ssl = wolfSSL_new(client_ctx);
    if (!ssl) {
        fprintf(stderr, "[ERROR] Failed to create SSL object\n");
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    if (wolfSSL_set_fd(ssl, client_sock) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to set socket\n");
        wolfSSL_free(ssl);
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    /* TLS handshake */
    if (wolfSSL_connect(ssl) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] TLS handshake failed\n");
        wolfSSL_free(ssl);
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    fprintf(stderr, "  ✓ Connected to server\n");
    
    /* Read header (magic bytes) */
    bytes_read = wolfSSL_read(ssl, buffer, 4);
    if (bytes_read != 4) {
        fprintf(stderr, "[ERROR] Failed to read header\n");
        wolfSSL_free(ssl);
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    /* Skip to correct size (need to read through all sizes to get to the right one) */
    size_t bytes_to_skip = 0;
    for (int i = 0; i < N_SIZES; i++) {
        if (PAYLOAD_SIZES[i] == payload_size) break;
        bytes_to_skip += PAYLOAD_SIZES[i] * 100;  /* 100 payloads per size */
    }
    
    /* Skip unwanted sizes */
    for (size_t skipped = 0; skipped < bytes_to_skip; ) {
        size_t to_read = (bytes_to_skip - skipped) < sizeof(buffer) ? 
                         (bytes_to_skip - skipped) : sizeof(buffer);
        bytes_read = wolfSSL_read(ssl, buffer, to_read);
        if (bytes_read <= 0) {
            fprintf(stderr, "[ERROR] Failed during skip\n");
            failed++;
            break;
        }
        skipped += bytes_read;
    }
    
    /* Measure Config A: Read only */
    fprintf(stderr, "  Measuring Config A...\n");
    result->iteration_times = (double *)malloc(iterations * sizeof(double));
    if (!result->iteration_times) {
        fprintf(stderr, "[ERROR] Memory allocation failed\n");
        return 1;
    }
    
    timer_start(&timer);
    
    for (int iter = 0; iter < iterations; iter++) {
        benchmark_timer_t iter_timer;
        timer_start(&iter_timer);
        
        bytes_read = wolfSSL_read(ssl, buffer, payload_size);
        if (bytes_read != (int)payload_size) {
            failed++;
        }
        
        timer_stop(&iter_timer);
        result->iteration_times[iter] = timer_us(&iter_timer);
        
        if ((iter + 1) % 100 == 0) {
            fprintf(stderr, "    [%d/%d]\n", iter + 1, iterations);
        }
    }
    
    timer_stop(&timer);
    
    result->payload_size = payload_size;
    result->config = 'A';
    result->iterations = iterations;
    result->total_time_us = timer_us(&timer);
    result->avg_time_per_iter = result->total_time_us / iterations;
    result->stddev = calculate_stddev(result->iteration_times, iterations);
    result->failed_count = failed;
    
    fprintf(stderr, "  ✓ Config A complete\n");
    
    /* Cleanup */
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_sock);
    wolfSSL_CTX_free(client_ctx);
    free(result->iteration_times);
    
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Config B: tls_read_peek() + wolfSSL_read() */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_peek_and_read(const char *server_ip, size_t payload_size,
                                   int iterations, benchmark_result_t *result) {
    int client_sock, failed = 0;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *client_ctx;
    WOLFSSL *ssl;
    benchmark_timer_t timer;
    unsigned char buffer[8192];
    int bytes_read;
    
    fprintf(stderr, "[MB-2-B] tls_read_peek() + wolfSSL_read(): payload=%zu bytes, iterations=%d\n",
            payload_size, iterations);
    
    /* Create context */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client context\n");
        return 1;
    }
    
    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    
    /* Connect to server */
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        perror("socket");
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MB2_PORT);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    
    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    /* Create SSL connection */
    ssl = wolfSSL_new(client_ctx);
    if (!ssl) {
        fprintf(stderr, "[ERROR] Failed to create SSL object\n");
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    if (wolfSSL_set_fd(ssl, client_sock) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to set socket\n");
        wolfSSL_free(ssl);
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    /* TLS handshake */
    if (wolfSSL_connect(ssl) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[ERROR] TLS handshake failed\n");
        wolfSSL_free(ssl);
        close(client_sock);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }
    
    fprintf(stderr, "  ✓ Connected to server\n");
    
    /* Read header */
    bytes_read = wolfSSL_read(ssl, buffer, 4);
    if (bytes_read != 4) {
        fprintf(stderr, "[ERROR] Failed to read header\n");
        return 1;
    }
    
    /* Skip to correct size */
    size_t bytes_to_skip = 0;
    for (int i = 0; i < N_SIZES; i++) {
        if (PAYLOAD_SIZES[i] == payload_size) break;
        bytes_to_skip += PAYLOAD_SIZES[i] * 100;
    }
    
    for (size_t skipped = 0; skipped < bytes_to_skip; ) {
        size_t to_read = (bytes_to_skip - skipped) < sizeof(buffer) ? 
                         (bytes_to_skip - skipped) : sizeof(buffer);
        bytes_read = wolfSSL_read(ssl, buffer, to_read);
        if (bytes_read <= 0) {
            failed++;
            break;
        }
        skipped += bytes_read;
    }
    
    /* Measure Config B: Peek + Read */
    fprintf(stderr, "  Measuring Config B...\n");
    result->iteration_times = (double *)malloc(iterations * sizeof(double));
    if (!result->iteration_times) {
        fprintf(stderr, "[ERROR] Memory allocation failed\n");
        return 1;
    }
    
    timer_start(&timer);
    
    for (int iter = 0; iter < iterations; iter++) {
        benchmark_timer_t iter_timer;
        timer_start(&iter_timer);
        
        /* Simulate tls_read_peek() - read data once (decrypt) */
        unsigned char peek_buffer[8192];
        bytes_read = wolfSSL_read(ssl, peek_buffer, payload_size);
        if (bytes_read != (int)payload_size) {
            failed++;
        }
        
        /* wolfSSL_read() - read data again (decrypt again) */
        bytes_read = wolfSSL_read(ssl, buffer, payload_size);
        if (bytes_read != (int)payload_size) {
            failed++;
        }
        
        timer_stop(&iter_timer);
        result->iteration_times[iter] = timer_us(&iter_timer);
        
        if ((iter + 1) % 100 == 0) {
            fprintf(stderr, "    [%d/%d]\n", iter + 1, iterations);
        }
    }
    
    timer_stop(&timer);
    
    result->payload_size = payload_size;
    result->config = 'B';
    result->iterations = iterations;
    result->total_time_us = timer_us(&timer);
    result->avg_time_per_iter = result->total_time_us / iterations;
    result->stddev = calculate_stddev(result->iteration_times, iterations);
    result->failed_count = failed;
    
    fprintf(stderr, "  ✓ Config B complete\n");
    
    /* Cleanup */
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    close(client_sock);
    wolfSSL_CTX_free(client_ctx);
    free(result->iteration_times);
    
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <SERVER_IP>\n", argv[0]);
        fprintf(stderr, "Example: %s 192.168.1.105\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  MB-2 BENCHMARK - read_peek() vs wolfSSL_read() overhead  ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n\n");
    
    fprintf(stderr, "Server: %s:%d\n", server_ip, MB2_PORT);
    fprintf(stderr, "Payload sizes: 256B, 1KiB, 4KiB\n");
    fprintf(stderr, "Iterations per size: %d\n\n", ITERATIONS);
    
    wolfSSL_library_init();
    
    /* Test each payload size */
    for (int size_idx = 0; size_idx < N_SIZES; size_idx++) {
        size_t payload_size = PAYLOAD_SIZES[size_idx];
        benchmark_result_t result_a, result_b;
        
        fprintf(stderr, "\n════════════════════════════════════════════════════════════\n");
        fprintf(stderr, "Testing payload size: %s\n", SIZE_NAMES[size_idx]);
        fprintf(stderr, "════════════════════════════════════════════════════════════\n\n");
        
        /* Config A */
        if (benchmark_read_only(server_ip, payload_size, ITERATIONS, &result_a) != 0) {
            fprintf(stderr, "[ERROR] Config A benchmark failed\n");
            continue;
        }
        
        sleep(1);  /* Delay between benchmarks */
        
        /* Config B */
        if (benchmark_peek_and_read(server_ip, payload_size, ITERATIONS, &result_b) != 0) {
            fprintf(stderr, "[ERROR] Config B benchmark failed\n");
            continue;
        }
        
        /* Output CSV */
        printf("%zu,A,%d,%.2f,%.2f,%.2f,%d\n",
               payload_size, ITERATIONS, result_a.total_time_us,
               result_a.avg_time_per_iter, result_a.stddev, result_a.failed_count);
        
        printf("%zu,B,%d,%.2f,%.2f,%.2f,%d\n",
               payload_size, ITERATIONS, result_b.total_time_us,
               result_b.avg_time_per_iter, result_b.stddev, result_b.failed_count);
        
        /* Calculate overhead */
        double overhead_us = result_b.avg_time_per_iter - result_a.avg_time_per_iter;
        double overhead_pct = (overhead_us / result_a.avg_time_per_iter) * 100.0;
        
        fprintf(stderr, "\n>>> Results for %s:\n", SIZE_NAMES[size_idx]);
        fprintf(stderr, "    Config A (read only):    %.2f ± %.2f µs\n",
                result_a.avg_time_per_iter, result_a.stddev);
        fprintf(stderr, "    Config B (peek + read):  %.2f ± %.2f µs\n",
                result_b.avg_time_per_iter, result_b.stddev);
        fprintf(stderr, "    Overhead:                %.2f µs (%+.1f%%)\n",
                overhead_us, overhead_pct);
    }
    
    fprintf(stderr, "\n[✓] Benchmark complete. Results above in CSV format.\n");
    return 0;
}
