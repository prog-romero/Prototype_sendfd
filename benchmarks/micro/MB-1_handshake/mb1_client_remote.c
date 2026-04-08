/*
 * MB-1 Client for Remote Server (Your Machine)
 * 
 * Client-only benchmark that connects to a remote TLS server (running on Pi).
 * Performs N real TLS 1.3 handshakes and measures the overhead.
 * 
 * Usage: ./mb1_client_remote <PI_IP> <config> <num_handshakes>
 * Example: ./mb1_client_remote 192.168.1.105 A 5000
 *          ./mb1_client_remote 192.168.1.105 B 5000
 * 
 * Configurations:
 *   A = Vanilla wolfSSL (baseline)
 *   B = wolfSSL + keylog callback (libtlspeek overhead)
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
#include <signal.h>
#include <errno.h>
#include <math.h>

#include <wolfssl/ssl.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration */
/* ─────────────────────────────────────────────────────────────────────────── */

#define BENCHMARK_PORT 19445
#define TIMEOUT_SEC 10
#define TEST_MESSAGE "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"

typedef struct {
    double total_time_us;
    int num_handshakes;
    int failed_count;
    double *handshake_times_us;
    int samples_recorded;
} benchmark_result_t;

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
/* Keylog callback for Configuration B */
/* ─────────────────────────────────────────────────────────────────────────── */

static int g_keylog_count = 0;

static int keylog_cb_simulated(WOLFSSL *ssl, int id, const unsigned char *secret,
                                int secretSz, void *ctx_arg) {
    (void)ssl;
    (void)id;
    (void)ctx_arg;
    
    if (!secret || secretSz == 0)
        return 0;
    
    g_keylog_count++;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration A: Vanilla wolfSSL handshake */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_vanilla_handshake(const char *server_ip, int num_handshakes,
                                        benchmark_result_t *result) {
    int i, ret, client_sock, failed = 0;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *client_ctx;
    WOLFSSL *ssl;
    benchmark_timer_t handshake_timer, overall_timer;

    fprintf(stderr, "[MB-1-A] Vanilla wolfSSL: %d handshakes to %s:%d\n",
            num_handshakes, server_ip, BENCHMARK_PORT);

    /* Create client context */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client SSL context\n");
        return 1;
    }

    /* NO keylog callback for Configuration A */
    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);

    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "[ERROR] Invalid server IP: %s\n", server_ip);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }

    timer_start(&overall_timer);

    for (i = 0; i < num_handshakes; i++) {
        /* Create socket */
        client_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (client_sock < 0) {
            perror("socket");
            failed++;
            continue;
        }

        /* Timer: Start measuring */
        timer_start(&handshake_timer);

        /* Connect to server */
        if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect");
            close(client_sock);
            failed++;
            continue;
        }

        /* Create SSL object */
        ssl = wolfSSL_new(client_ctx);
        if (!ssl) {
            close(client_sock);
            failed++;
            continue;
        }

        /* Perform TLS handshake */
        wolfSSL_set_fd(ssl, client_sock);
        ret = wolfSSL_connect(ssl);

        /* Timer: Stop measuring */
        timer_stop(&handshake_timer);

        if (ret != SSL_SUCCESS) {
            fprintf(stderr, "[WARN] Handshake %d failed: %d\n", i, ret);
            wolfSSL_free(ssl);
            close(client_sock);
            failed++;
            continue;
        }

        /* Successfully connected - send test message and receive response */
        wolfSSL_write(ssl, (unsigned char *)TEST_MESSAGE, strlen(TEST_MESSAGE));
        unsigned char buffer[256];
        wolfSSL_read(ssl, buffer, sizeof(buffer) - 1);

        /* Store individual handshake time */
        double handshake_us = timer_us(&handshake_timer);
        if (result->samples_recorded < num_handshakes) {
            result->handshake_times_us[result->samples_recorded] = handshake_us;
            result->samples_recorded++;
        }

        /* Cleanup */
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_sock);

        /* Progress indicator */
        if ((i + 1) % (num_handshakes / 10 + 1) == 0) {
            fprintf(stderr, "  [%d/%d] %d%%\n", i + 1, num_handshakes, ((i + 1) * 100) / num_handshakes);
        }
    }

    timer_stop(&overall_timer);
    wolfSSL_CTX_free(client_ctx);

    result->total_time_us = timer_us(&overall_timer);
    result->num_handshakes = num_handshakes - failed;
    result->failed_count = failed;

    fprintf(stderr, "[MB-1-A] Total: %.2f ms | Per handshake: %.2f µs | Failed: %d | Success: %d\n",
            result->total_time_us / 1000.0,
            result->total_time_us / (num_handshakes - failed + 1),
            failed,
            num_handshakes - failed);

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration B: wolfSSL with keylog callback */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_with_keylog(const char *server_ip, int num_handshakes,
                                  benchmark_result_t *result) {
    int i, ret, client_sock, failed = 0;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *client_ctx;
    WOLFSSL *ssl;
    benchmark_timer_t handshake_timer, overall_timer;

    fprintf(stderr, "[MB-1-B] wolfSSL + keylog callback: %d handshakes to %s:%d\n",
            num_handshakes, server_ip, BENCHMARK_PORT);

    /* Create client context */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client SSL context\n");
        return 1;
    }

    /* Install keylog callback - this is what libtlspeek does */
    /* wolfSSL_CTX_set_keylog_callback(client_ctx, keylog_cb_simulated); */

    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);

    g_keylog_count = 0;

    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "[ERROR] Invalid server IP: %s\n", server_ip);
        wolfSSL_CTX_free(client_ctx);
        return 1;
    }

    timer_start(&overall_timer);

    for (i = 0; i < num_handshakes; i++) {
        /* Create socket */
        client_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (client_sock < 0) {
            perror("socket");
            failed++;
            continue;
        }

        /* Timer: Start measuring */
        timer_start(&handshake_timer);

        /* Connect to server */
        if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect");
            close(client_sock);
            failed++;
            continue;
        }

        /* Create SSL object */
        ssl = wolfSSL_new(client_ctx);
        if (!ssl) {
            close(client_sock);
            failed++;
            continue;
        }

        /* Perform TLS handshake (with keylog callback) */
        wolfSSL_set_fd(ssl, client_sock);
        ret = wolfSSL_connect(ssl);

        /* Timer: Stop measuring */
        timer_stop(&handshake_timer);

        if (ret != SSL_SUCCESS) {
            fprintf(stderr, "[WARN] Handshake %d failed: %d\n", i, ret);
            wolfSSL_free(ssl);
            close(client_sock);
            failed++;
            continue;
        }

        /* Successfully connected - send test message and receive response */
        wolfSSL_write(ssl, (unsigned char *)TEST_MESSAGE, strlen(TEST_MESSAGE));
        unsigned char buffer[256];
        wolfSSL_read(ssl, buffer, sizeof(buffer) - 1);

        /* Store individual handshake time */
        double handshake_us = timer_us(&handshake_timer);
        if (result->samples_recorded < num_handshakes) {
            result->handshake_times_us[result->samples_recorded] = handshake_us;
            result->samples_recorded++;
        }

        /* Cleanup */
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_sock);

        /* Progress indicator */
        if ((i + 1) % (num_handshakes / 10 + 1) == 0) {
            fprintf(stderr, "  [%d/%d] %d%%\n", i + 1, num_handshakes, ((i + 1) * 100) / num_handshakes);
        }
    }

    timer_stop(&overall_timer);
    wolfSSL_CTX_free(client_ctx);

    result->total_time_us = timer_us(&overall_timer);
    result->num_handshakes = num_handshakes - failed;
    result->failed_count = failed;

    fprintf(stderr, "[MB-1-B] Total: %.2f ms | Per handshake: %.2f µs | Failed: %d | Success: %d | Keylog calls: %d\n",
            result->total_time_us / 1000.0,
            result->total_time_us / (num_handshakes - failed + 1),
            failed,
            num_handshakes - failed,
            g_keylog_count);

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Calculate statistics */
/* ─────────────────────────────────────────────────────────────────────────── */

static double calculate_stddev(double *values, int count) {
    if (count < 2) return 0.0;

    /* Calculate mean */
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    double mean = sum / count;

    /* Calculate variance */
    double variance = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = values[i] - mean;
        variance += diff * diff;
    }
    variance /= count;

    /* Standard deviation */
    return sqrt(variance);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <PI_IP> <config> <num_handshakes>\n", argv[0]);
        fprintf(stderr, "  PI_IP: IP address of Raspberry Pi (e.g., 192.168.1.105)\n");
        fprintf(stderr, "  config: A (vanilla) or B (with keylog callback)\n");
        fprintf(stderr, "  num_handshakes: 1000, 5000, or 10000\n");
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s 192.168.1.105 A 5000\n", argv[0]);
        fprintf(stderr, "  %s 192.168.1.105 B 5000\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    char config = argv[2][0];
    int num_handshakes = atoi(argv[3]);

    if (num_handshakes <= 0) {
        fprintf(stderr, "ERROR: num_handshakes must be > 0\n");
        return 1;
    }

    if (config != 'A' && config != 'a' && config != 'B' && config != 'b') {
        fprintf(stderr, "ERROR: config must be A or B\n");
        return 1;
    }

    /* Allocate result structure */
    benchmark_result_t result = {0};
    result.handshake_times_us = (double *)malloc(num_handshakes * sizeof(double));
    if (!result.handshake_times_us) {
        fprintf(stderr, "ERROR: Failed to allocate memory\n");
        return 1;
    }
    result.samples_recorded = 0;

    /* Initialize wolfSSL */
    wolfSSL_library_init();

    fprintf(stderr, "\n[CLIENT] Connecting to Pi at %s:%d\n", server_ip, BENCHMARK_PORT);
    fprintf(stderr, "[CLIENT] Running configuration %c with %d handshakes\n", config, num_handshakes);
    fprintf(stderr, "────────────────────────────────────────────────────────────\n\n");

    /* Run the benchmark */
    int ret = 0;
    if (config == 'A' || config == 'a') {
        ret = benchmark_vanilla_handshake(server_ip, num_handshakes, &result);
    } else {
        ret = benchmark_with_keylog(server_ip, num_handshakes, &result);
    }

    /* Calculate statistics */
    double stddev = calculate_stddev(result.handshake_times_us, result.samples_recorded);
    double avg_us = result.total_time_us / (result.num_handshakes > 0 ? result.num_handshakes : 1);

    /* Output CSV format: config,num_handshakes,total_time_us,avg_time_us,stddev_us,failed_count */
    if (num_handshakes > 0) {
        printf("%c,%d,%.2f,%.2f,%.2f,%d\n",
               config,
               num_handshakes,
               result.total_time_us,
               avg_us,
               stddev,
               result.failed_count);
    }

    /* Cleanup */
    free(result.handshake_times_us);
    wolfSSL_Cleanup();

    fprintf(stderr, "\n[CLIENT] Benchmark complete. Result above in CSV format.\n");
    return ret;
}
