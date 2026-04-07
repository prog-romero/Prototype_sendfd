/*
 * MB-1: TLS 1.3 Handshake Rate Benchmark - REAL Handshakes
 * 
 * Measures the overhead of libtlspeek's key extraction (keylog callback + HKDF)
 * compared to a vanilla wolfSSL handshake.
 *
 * Method: Perform N ∈ {1000, 5000, 10000} REAL TLS 1.3 handshakes.
 * 
 * Configurations:
 *   (A) vanilla wolfSSL           — Normal TLS 1.3 handshake
 *   (B) wolfSSL + keylog callback — Handshake with key extraction (libtlspeek)
 *
 * Architecture:
 *   - Server thread: Listens on localhost:19445, accepts connections, performs TLS accept
 *   - Client loop: Connects N times, performs TLS handshake, sends/receives data
 *   - Timer: Measures from socket connect() to wolfSSL_connect() completion
 *
 * Output: CSV with benchmark results
 * Usage: ./mb1_handshake <config_A_or_B> <num_handshakes>
 *        ./mb1_handshake A 1000
 *        ./mb1_handshake B 1000
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
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

#include <wolfssl/ssl.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration */
/* ─────────────────────────────────────────────────────────────────────────── */

#define BENCHMARK_PORT 19445
#define BENCHMARK_ADDR "127.0.0.1"
#define TIMEOUT_SEC 10
#define TEST_MESSAGE "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
#define TEST_RESPONSE "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nOK\r\n"

/* Certificate paths - from wolfSSL test certs */
#define CERT_FILE "/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/certs/server-cert.der"
#define KEY_FILE "/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/certs/server-key.der"

typedef struct {
    double total_time_us;
    int num_handshakes;
    int failed_count;
    double *handshake_times_us;  /* Array of individual handshake times */
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
/* Server context (global for simplicity) */
/* ─────────────────────────────────────────────────────────────────────────── */

static volatile int server_shutdown = 0;
static WOLFSSL_CTX *g_server_ctx = NULL;
static int g_server_socket = -1;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Keylog callback for Configuration B */
/* ─────────────────────────────────────────────────────────────────────────── */

static int g_keylog_count = 0;

static int keylog_cb_simulated(WOLFSSL *ssl, int id, const unsigned char *secret,
                                int secretSz, void *ctx_arg) {
    /* Simulates libtlspeek's keylog callback - called during handshake */
    (void)ssl;
    (void)id;
    (void)ctx_arg;
    
    if (!secret || secretSz == 0)
        return 0;
    
    g_keylog_count++;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* TLS Server Thread */
/* ─────────────────────────────────────────────────────────────────────────── */

static void *tls_server_thread(void *arg) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    WOLFSSL *ssl;
    char buffer[256];
    int numbytes;

    /* Server socket setup */
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return NULL;
    }

    /* Allow socket reuse */
    int reuse = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Bind to localhost */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(BENCHMARK_ADDR);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return NULL;
    }

    if (listen(server_sock, 128) < 0) {
        perror("listen");
        close(server_sock);
        return NULL;
    }

    g_server_socket = server_sock;
    fprintf(stderr, "[SERVER] Listening on %s:%d\n", BENCHMARK_ADDR, BENCHMARK_PORT);

    /* Accept connections until shutdown */
    while (!server_shutdown) {
        client_addr_len = sizeof(client_addr);
        
        /* Set accept timeout */
        struct timeval tv;
        tv.tv_sec = 1;  /* 1 second timeout */
        tv.tv_usec = 0;
        setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  /* Timeout, retry */
            }
            if (server_shutdown) break;
            perror("accept");
            continue;
        }

        /* Create SSL connection on accepted socket */
        ssl = wolfSSL_new(g_server_ctx);
        if (!ssl) {
            fprintf(stderr, "[SERVER] wolfSSL_new failed\n");
            close(client_sock);
            continue;
        }

        /* Set socket and perform TLS accept */
        wolfSSL_set_fd(ssl, client_sock);
        if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
            fprintf(stderr, "[SERVER] wolfSSL_accept failed\n");
            wolfSSL_free(ssl);
            close(client_sock);
            continue;
        }

        /* Receive data from client */
        numbytes = wolfSSL_read(ssl, (unsigned char*)buffer, sizeof(buffer) - 1);
        if (numbytes > 0) {
            buffer[numbytes] = '\0';
            /* Send response */
            wolfSSL_write(ssl, (unsigned char *)TEST_RESPONSE, strlen(TEST_RESPONSE));
        }

        /* Close connection */
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        close(client_sock);
    }

    close(server_sock);
    fprintf(stderr, "[SERVER] Shutdown\n");
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Configuration A: Vanilla wolfSSL (No keylog) */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_vanilla_handshake(int num_handshakes, benchmark_result_t *result) {
    WOLFSSL_CTX *client_ctx = NULL;
    WOLFSSL *ssl = NULL;
    int client_sock;
    struct sockaddr_in server_addr;
    benchmark_timer_t overall_timer, handshake_timer;
    int i, failed = 0;
    int ret;

    fprintf(stderr, "[MB-1-A] Vanilla wolfSSL: %d handshakes\n", num_handshakes);

    /* Create client context WITHOUT keylog */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client SSL context\n");
        return -1;
    }

    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);

    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(BENCHMARK_ADDR);

    timer_start(&overall_timer);

    for (i = 0; i < num_handshakes; i++) {
        /* Create socket */
        client_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (client_sock < 0) {
            perror("socket");
            failed++;
            continue;
        }

        /* Timer: Start measuring connection time */
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
        
        /* Timer: Stop measuring (handshake complete) */
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
/* Configuration B: wolfSSL + Keylog Callback (libtlspeek simulation) */
/* ─────────────────────────────────────────────────────────────────────────── */

static int benchmark_with_keylog(int num_handshakes, benchmark_result_t *result) {
    WOLFSSL_CTX *client_ctx = NULL;
    WOLFSSL *ssl = NULL;
    int client_sock;
    struct sockaddr_in server_addr;
    benchmark_timer_t overall_timer, handshake_timer;
    int i, failed = 0;
    int ret;

    fprintf(stderr, "[MB-1-B] With keylog callback: %d handshakes\n", num_handshakes);

    /* Create client context WITH keylog callback */
    client_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!client_ctx) {
        fprintf(stderr, "[ERROR] Failed to create client SSL context\n");
        return -1;
    }

    /* Install keylog callback - this is what libtlspeek does */
    wolfSSL_CTX_set_keylog_callback(client_ctx, keylog_cb_simulated);

    wolfSSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);

    g_keylog_count = 0;

    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BENCHMARK_PORT);
    server_addr.sin_addr.s_addr = inet_addr(BENCHMARK_ADDR);

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
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <config> <num_handshakes>\n", argv[0]);
        fprintf(stderr, "  config: A (vanilla) or B (with keylog)\n");
        fprintf(stderr, "  num_handshakes: 1000, 5000, or 10000\n");
        return 1;
    }

    char config = argv[1][0];
    int num_handshakes = atoi(argv[2]);

    if (num_handshakes <= 0) {
        fprintf(stderr, "ERROR: num_handshakes must be > 0\n");
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

    /* Create server context */
    g_server_ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!g_server_ctx) {
        fprintf(stderr, "[ERROR] Failed to create server SSL context\n");
        free(result.handshake_times_us);
        return 1;
    }

    /* Load server certificate and key from files */
    if (wolfSSL_CTX_use_certificate_file(g_server_ctx, CERT_FILE, SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server certificate from %s\n", CERT_FILE);
        wolfSSL_CTX_free(g_server_ctx);
        free(result.handshake_times_us);
        return 1;
    }

    if (wolfSSL_CTX_use_PrivateKey_file(g_server_ctx, KEY_FILE, SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        fprintf(stderr, "[ERROR] Failed to load server key from %s\n", KEY_FILE);
        wolfSSL_CTX_free(g_server_ctx);
        free(result.handshake_times_us);
        return 1;
    }

    /* Start server thread */
    pthread_t server_tid;
    if (pthread_create(&server_tid, NULL, tls_server_thread, NULL) != 0) {
        fprintf(stderr, "[ERROR] Failed to create server thread\n");
        wolfSSL_CTX_free(g_server_ctx);
        free(result.handshake_times_us);
        return 1;
    }

    /* Give server time to start */
    usleep(100000);  /* 100ms */

    /* Run the benchmark */
    int ret = 0;
    if (config == 'A' || config == 'a') {
        ret = benchmark_vanilla_handshake(num_handshakes, &result);
    } else if (config == 'B' || config == 'b') {
        ret = benchmark_with_keylog(num_handshakes, &result);
    } else {
        fprintf(stderr, "ERROR: config must be A or B\n");
        ret = 1;
    }

    /* Shutdown server */
    server_shutdown = 1;
    pthread_join(server_tid, NULL);

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
    wolfSSL_CTX_free(g_server_ctx);
    free(result.handshake_times_us);

    return ret;
}
