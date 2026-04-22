#define _GNU_SOURCE

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/bench2_rdtsc.h"

#define DEFAULT_ITERATIONS 1000
#define DEFAULT_WARMUP 100
#define DEFAULT_SIZES "64,256,1024,4096,16384,65536,131072,262144,524288,1048576"
#define DEFAULT_TLS_VERSION "1.3"
#define DEFAULT_TLS13_CIPHER "TLS13-AES128-GCM-SHA256"
#define DEFAULT_TLS12_CIPHER "ECDHE-RSA-AES128-GCM-SHA256"
#define DEFAULT_QUEUE_CAP (8U * 1024U * 1024U)
#define MAX_APP_WRITE_CHUNK (16U * 1024U)

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} memio_queue_t;

typedef struct {
    memio_queue_t c2s;
    memio_queue_t s2c;
} memio_transport_t;

typedef struct {
    memio_transport_t *transport;
    int is_server;
} memio_endpoint_t;

typedef struct {
    WOLFSSL_CTX *client_ctx;
    WOLFSSL_CTX *server_ctx;
    WOLFSSL *client_ssl;
    WOLFSSL *server_ssl;
    memio_transport_t transport;
    memio_endpoint_t client_ep;
    memio_endpoint_t server_ep;
} ssl_pair_t;

typedef struct {
    const char *server_cert;
    const char *server_key;
    const char *ca_cert;
    const char *tls_version;
    const char *cipher;
    const char *sizes_csv;
    const char *summary_out;
    const char *raw_out;
    int iterations;
    int warmup;
    int core;
} bench_config_t;

typedef struct {
    uint64_t min_ns;
    double mean_ns;
    double stddev_ns;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
} stats_t;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --server-cert PATH   Server certificate PEM\n"
        "  --server-key PATH    Server private key PEM\n"
        "  --ca-cert PATH       CA certificate PEM for the client\n"
        "  --tls-version VER    1.2 or 1.3 (default: %s)\n"
        "  --cipher NAME        Cipher suite string\n"
        "  --sizes LIST         Comma-separated payload sizes in bytes\n"
        "  --iterations N       Measured iterations per size (default: %d)\n"
        "  --warmup N           Warmup iterations per size (default: %d)\n"
        "  --core N             Pin process to CPU core N\n"
        "  --summary-out PATH   Summary CSV path\n"
        "  --raw-out PATH       Raw CSV path\n",
        prog, DEFAULT_TLS_VERSION, DEFAULT_ITERATIONS, DEFAULT_WARMUP);
}

static int pin_to_core(int core)
{
#ifdef __linux__
    cpu_set_t set;

    if (core < 0) {
        return 0;
    }

    CPU_ZERO(&set);
    CPU_SET((unsigned int)core, &set);
    return sched_setaffinity(0, sizeof(set), &set);
#else
    (void)core;
    return 0;
#endif
}

static int queue_init(memio_queue_t *queue, size_t cap)
{
    memset(queue, 0, sizeof(*queue));

    queue->data = (unsigned char *)malloc(cap);
    if (!queue->data) {
        return -1;
    }

    queue->cap = cap;
    return 0;
}

static void queue_free(memio_queue_t *queue)
{
    free(queue->data);
    memset(queue, 0, sizeof(*queue));
}

static int queue_reserve(memio_queue_t *queue, size_t additional)
{
    size_t new_cap;
    unsigned char *new_data;

    if (additional <= queue->cap - queue->len) {
        return 0;
    }

    new_cap = queue->cap ? queue->cap : 4096U;
    while (additional > new_cap - queue->len) {
        if (new_cap > SIZE_MAX / 2U) {
            return -1;
        }
        new_cap *= 2U;
    }

    new_data = (unsigned char *)realloc(queue->data, new_cap);
    if (!new_data) {
        return -1;
    }

    queue->data = new_data;
    queue->cap = new_cap;
    return 0;
}

static int queue_push(memio_queue_t *queue, const unsigned char *data, int sz)
{
    if (sz < 0) {
        return -1;
    }
    if (queue_reserve(queue, (size_t)sz) != 0) {
        return -1;
    }

    memcpy(queue->data + queue->len, data, (size_t)sz);
    queue->len += (size_t)sz;
    return 0;
}

static int queue_pop(memio_queue_t *queue, unsigned char *data, int sz)
{
    int read_sz;

    if (queue->len == 0) {
        return 0;
    }

    read_sz = (int)((size_t)sz < queue->len ? (size_t)sz : queue->len);
    if ((size_t)read_sz > queue->len) {
        return -1;
    }

    memcpy(data, queue->data, (size_t)read_sz);
    memmove(queue->data, queue->data + read_sz, queue->len - (size_t)read_sz);
    queue->len -= (size_t)read_sz;

    return read_sz;
}

static int memio_send_cb(WOLFSSL *ssl, char *data, int sz, void *ctx)
{
    memio_endpoint_t *endpoint = (memio_endpoint_t *)ctx;
    memio_queue_t *queue;

    (void)ssl;

    queue = endpoint->is_server ? &endpoint->transport->s2c : &endpoint->transport->c2s;
    if (queue_push(queue, (const unsigned char *)data, sz) != 0) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }
    return sz;
}

static int memio_recv_cb(WOLFSSL *ssl, char *data, int sz, void *ctx)
{
    memio_endpoint_t *endpoint = (memio_endpoint_t *)ctx;
    memio_queue_t *queue;
    int ret;

    (void)ssl;

    queue = endpoint->is_server ? &endpoint->transport->c2s : &endpoint->transport->s2c;
    ret = queue_pop(queue, (unsigned char *)data, sz);
    if (ret < 0) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    if (ret == 0) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    return ret;
}

static void set_default_cipher(bench_config_t *cfg)
{
    if (cfg->cipher != NULL) {
        return;
    }

    if (strcmp(cfg->tls_version, "1.2") == 0) {
        cfg->cipher = DEFAULT_TLS12_CIPHER;
    }
    else {
        cfg->cipher = DEFAULT_TLS13_CIPHER;
    }
}

static int is_retryable_wolfssl_error(int err)
{
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        return 1;
    }

#ifdef MP_WOULDBLOCK
    if (err == MP_WOULDBLOCK || err == WC_NO_ERR_TRACE(MP_WOULDBLOCK)) {
        return 1;
    }
#endif

#ifdef WC_PENDING_E
    if (err == WC_PENDING_E || err == WC_NO_ERR_TRACE(WC_PENDING_E)) {
        return 1;
    }
#endif

    return 0;
}

static void print_wolfssl_error(const char *op, WOLFSSL *ssl, int ret)
{
    int err = wolfSSL_get_error(ssl, ret);
    char err_buf[WOLFSSL_MAX_ERROR_SZ];

    fprintf(stderr,
            "%s failed: ret=%d err=%d (%s)\n",
            op,
            ret,
            err,
            wolfSSL_ERR_error_string((unsigned long)err, err_buf));
}

static int ssl_write_all(WOLFSSL *ssl, const unsigned char *buf, size_t len);
static int ssl_read_exact(WOLFSSL *ssl, unsigned char *buf, size_t len);

static int do_handshake(ssl_pair_t *pair)
{
    int client_done = 0;
    int server_done = 0;
    int rounds = 32;

    while (!(client_done && server_done) && rounds-- > 0) {
        if (!client_done) {
            int ret = wolfSSL_connect(pair->client_ssl);
            if (ret == WOLFSSL_SUCCESS) {
                client_done = 1;
            }
            else {
                int err = wolfSSL_get_error(pair->client_ssl, ret);
                if (!is_retryable_wolfssl_error(err)) {
                    print_wolfssl_error("client handshake", pair->client_ssl, ret);
                    return -1;
                }
            }
        }

        if (!server_done) {
            int ret = wolfSSL_accept(pair->server_ssl);
            if (ret == WOLFSSL_SUCCESS) {
                server_done = 1;
            }
            else {
                int err = wolfSSL_get_error(pair->server_ssl, ret);
                if (!is_retryable_wolfssl_error(err)) {
                    print_wolfssl_error("server handshake", pair->server_ssl, ret);
                    return -1;
                }
            }
        }
    }

    if (!(client_done && server_done)) {
        fprintf(stderr, "handshake did not complete\n");
        return -1;
    }

    return 0;
}

static int quiesce_connection(ssl_pair_t *pair)
{
    unsigned char tx = 0;
    unsigned char rx = 0;
    int round;

    for (round = 0; round < 8; ++round) {
        tx = (unsigned char)(0xA0 + round);
        if (ssl_write_all(pair->server_ssl, &tx, 1) != 0) {
            fprintf(stderr, "failed to send server quiesce byte\n");
            return -1;
        }
        if (ssl_read_exact(pair->client_ssl, &rx, 1) != 0 || rx != tx) {
            fprintf(stderr, "failed to receive server quiesce byte\n");
            return -1;
        }

        tx = (unsigned char)(0x50 + round);
        if (ssl_write_all(pair->client_ssl, &tx, 1) != 0) {
            fprintf(stderr, "failed to send client quiesce byte\n");
            return -1;
        }
        if (ssl_read_exact(pair->server_ssl, &rx, 1) != 0 || rx != tx) {
            fprintf(stderr, "failed to receive client quiesce byte\n");
            return -1;
        }

        if (pair->transport.s2c.len == 0 && pair->transport.c2s.len == 0) {
            return 0;
        }
    }

    fprintf(stderr,
            "connection did not quiesce: s2c=%zu c2s=%zu\n",
            pair->transport.s2c.len,
            pair->transport.c2s.len);
    return -1;
}

static void free_ssl_pair(ssl_pair_t *pair)
{
    if (pair->client_ssl) {
        wolfSSL_set_quiet_shutdown(pair->client_ssl, 1);
        wolfSSL_free(pair->client_ssl);
    }
    if (pair->server_ssl) {
        wolfSSL_set_quiet_shutdown(pair->server_ssl, 1);
        wolfSSL_free(pair->server_ssl);
    }
    if (pair->client_ctx) {
        wolfSSL_CTX_free(pair->client_ctx);
    }
    if (pair->server_ctx) {
        wolfSSL_CTX_free(pair->server_ctx);
    }

    queue_free(&pair->transport.c2s);
    queue_free(&pair->transport.s2c);
    memset(pair, 0, sizeof(*pair));
}

static int setup_ssl_pair(ssl_pair_t *pair, const bench_config_t *cfg, size_t max_payload)
{
    WOLFSSL_METHOD *client_method = NULL;
    WOLFSSL_METHOD *server_method = NULL;
    size_t queue_cap = DEFAULT_QUEUE_CAP;

    memset(pair, 0, sizeof(*pair));

    if (max_payload * 4U + (1024U * 1024U) > queue_cap) {
        queue_cap = max_payload * 4U + (1024U * 1024U);
    }

    if (queue_init(&pair->transport.c2s, queue_cap) != 0 ||
        queue_init(&pair->transport.s2c, queue_cap) != 0) {
        fprintf(stderr, "failed to allocate memory transport\n");
        free_ssl_pair(pair);
        return -1;
    }

    if (strcmp(cfg->tls_version, "1.2") == 0) {
        client_method = wolfTLSv1_2_client_method();
        server_method = wolfTLSv1_2_server_method();
    }
    else if (strcmp(cfg->tls_version, "1.3") == 0) {
        client_method = wolfTLSv1_3_client_method();
        server_method = wolfTLSv1_3_server_method();
    }
    else {
        fprintf(stderr, "unsupported TLS version: %s\n", cfg->tls_version);
        free_ssl_pair(pair);
        return -1;
    }

    pair->client_ctx = wolfSSL_CTX_new(client_method);
    pair->server_ctx = wolfSSL_CTX_new(server_method);
    if (!pair->client_ctx || !pair->server_ctx) {
        fprintf(stderr, "wolfSSL_CTX_new failed\n");
        free_ssl_pair(pair);
        return -1;
    }

    wolfSSL_SetIORecv(pair->client_ctx, memio_recv_cb);
    wolfSSL_SetIOSend(pair->client_ctx, memio_send_cb);
    wolfSSL_SetIORecv(pair->server_ctx, memio_recv_cb);
    wolfSSL_SetIOSend(pair->server_ctx, memio_send_cb);

    if (wolfSSL_CTX_load_verify_locations(pair->client_ctx, cfg->ca_cert, NULL) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "failed to load CA cert: %s\n", cfg->ca_cert);
        free_ssl_pair(pair);
        return -1;
    }
    if (wolfSSL_CTX_use_certificate_file(pair->server_ctx, cfg->server_cert, SSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "failed to load server cert: %s\n", cfg->server_cert);
        free_ssl_pair(pair);
        return -1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(pair->server_ctx, cfg->server_key, SSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "failed to load server key: %s\n", cfg->server_key);
        free_ssl_pair(pair);
        return -1;
    }

    wolfSSL_CTX_set_verify(pair->client_ctx, WOLFSSL_VERIFY_PEER, NULL);
    wolfSSL_CTX_set_verify(pair->server_ctx, WOLFSSL_VERIFY_NONE, NULL);

    if (cfg->cipher != NULL) {
        if (wolfSSL_CTX_set_cipher_list(pair->client_ctx, cfg->cipher) != WOLFSSL_SUCCESS ||
            wolfSSL_CTX_set_cipher_list(pair->server_ctx, cfg->cipher) != WOLFSSL_SUCCESS) {
            fprintf(stderr, "failed to set cipher list: %s\n", cfg->cipher);
            free_ssl_pair(pair);
            return -1;
        }
    }

    if (strcmp(cfg->tls_version, "1.3") == 0) {
        (void)wolfSSL_CTX_no_ticket_TLSv13(pair->server_ctx);
    }

    pair->client_ssl = wolfSSL_new(pair->client_ctx);
    pair->server_ssl = wolfSSL_new(pair->server_ctx);
    if (!pair->client_ssl || !pair->server_ssl) {
        fprintf(stderr, "wolfSSL_new failed\n");
        free_ssl_pair(pair);
        return -1;
    }

    pair->client_ep.transport = &pair->transport;
    pair->client_ep.is_server = 0;
    pair->server_ep.transport = &pair->transport;
    pair->server_ep.is_server = 1;

    wolfSSL_SetIOReadCtx(pair->client_ssl, &pair->client_ep);
    wolfSSL_SetIOWriteCtx(pair->client_ssl, &pair->client_ep);
    wolfSSL_SetIOReadCtx(pair->server_ssl, &pair->server_ep);
    wolfSSL_SetIOWriteCtx(pair->server_ssl, &pair->server_ep);

    if (strcmp(cfg->tls_version, "1.3") == 0) {
        (void)wolfSSL_no_ticket_TLSv13(pair->server_ssl);
    }

    if (do_handshake(pair) != 0) {
        free_ssl_pair(pair);
        return -1;
    }

    if (quiesce_connection(pair) != 0) {
        free_ssl_pair(pair);
        return -1;
    }

    return 0;
}

static int ssl_write_all(WOLFSSL *ssl, const unsigned char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        size_t chunk_len = len - written;
        int ret;

        if (chunk_len > MAX_APP_WRITE_CHUNK) {
            chunk_len = MAX_APP_WRITE_CHUNK;
        }

        ret = wolfSSL_write(ssl, buf + written, (int)chunk_len);
        if (ret > 0) {
            written += (size_t)ret;
            continue;
        }

        {
            int err = wolfSSL_get_error(ssl, ret);

            if (is_retryable_wolfssl_error(err)) {
                continue;
            }

            print_wolfssl_error("wolfSSL_write", ssl, ret);
            return -1;
        }
    }

    return 0;
}

static int ssl_read_exact(WOLFSSL *ssl, unsigned char *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {
        int ret = wolfSSL_read(ssl, buf + total, (int)(len - total));
        if (ret > 0) {
            total += (size_t)ret;
            continue;
        }

        {
            int err = wolfSSL_get_error(ssl, ret);

            if (is_retryable_wolfssl_error(err)) {
                continue;
            }

            print_wolfssl_error("wolfSSL_read", ssl, ret);
            return -1;
        }
    }

    return 0;
}

static int run_one_iteration(ssl_pair_t *pair,
                             const unsigned char *plaintext,
                             unsigned char *recv_buf,
                             size_t payload_size,
                             uint64_t *delta_cycles_out,
                             uint64_t *cntfrq_out,
                             uint64_t *delta_ns_out)
{
    uint64_t top1;
    uint64_t top2;
    uint64_t cntfrq;
    uint64_t delta_cycles;
    uint64_t delta_ns;

    if (ssl_write_all(pair->server_ssl, plaintext, payload_size) != 0) {
        return -1;
    }

    cntfrq = bench2_cntfrq();
    top1 = bench2_rdtsc();
    if (ssl_read_exact(pair->client_ssl, recv_buf, payload_size) != 0) {
        return -1;
    }
    top2 = bench2_rdtsc();

    if (memcmp(plaintext, recv_buf, payload_size) != 0) {
        fprintf(stderr, "plaintext mismatch after decryption\n");
        return -1;
    }
    if (pair->transport.s2c.len != 0 || pair->transport.c2s.len != 0) {
        fprintf(stderr, "transport buffers not empty after iteration\n");
        return -1;
    }

    delta_cycles = top2 - top1;
    delta_ns = (uint64_t)(((long double)delta_cycles * 1000000000.0L) / (long double)cntfrq);

    if (delta_cycles_out) {
        *delta_cycles_out = delta_cycles;
    }
    if (cntfrq_out) {
        *cntfrq_out = cntfrq;
    }
    if (delta_ns_out) {
        *delta_ns_out = delta_ns;
    }

    return 0;
}

static int cmp_u64(const void *lhs, const void *rhs)
{
    uint64_t a = *(const uint64_t *)lhs;
    uint64_t b = *(const uint64_t *)rhs;

    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static stats_t compute_stats(const uint64_t *values, size_t count)
{
    stats_t stats;
    uint64_t *sorted;
    long double sum = 0.0L;
    long double var = 0.0L;
    size_t i;

    memset(&stats, 0, sizeof(stats));
    if (count == 0) {
        return stats;
    }

    sorted = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!sorted) {
        fprintf(stderr, "failed to allocate stats buffer\n");
        return stats;
    }

    memcpy(sorted, values, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), cmp_u64);

    stats.min_ns = sorted[0];
    stats.max_ns = sorted[count - 1];
    stats.p50_ns = sorted[(count - 1) * 50 / 100];
    stats.p95_ns = sorted[(count - 1) * 95 / 100];
    stats.p99_ns = sorted[(count - 1) * 99 / 100];

    for (i = 0; i < count; ++i) {
        sum += (long double)values[i];
    }
    stats.mean_ns = (double)(sum / (long double)count);

    if (count > 1) {
        for (i = 0; i < count; ++i) {
            long double diff = (long double)values[i] - (long double)stats.mean_ns;
            var += diff * diff;
        }
        stats.stddev_ns = sqrt((double)(var / (long double)(count - 1)));
    }

    free(sorted);
    return stats;
}

static int parse_sizes(const char *csv, size_t **sizes_out, size_t *count_out, size_t *max_size_out)
{
    char *copy;
    char *saveptr = NULL;
    char *tok;
    size_t *sizes = NULL;
    size_t count = 0;
    size_t cap = 8;
    size_t max_size = 0;

    copy = strdup(csv);
    if (!copy) {
        return -1;
    }

    sizes = (size_t *)malloc(cap * sizeof(size_t));
    if (!sizes) {
        free(copy);
        return -1;
    }

    for (tok = strtok_r(copy, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
        char *end = NULL;
        unsigned long long value;

        while (*tok == ' ' || *tok == '\t') {
            ++tok;
        }
        if (*tok == '\0') {
            continue;
        }

        errno = 0;
        value = strtoull(tok, &end, 10);
        if (errno != 0 || end == tok || *end != '\0' || value == 0ULL) {
            free(copy);
            free(sizes);
            return -1;
        }

        if (count == cap) {
            size_t new_cap = cap * 2;
            size_t *tmp = (size_t *)realloc(sizes, new_cap * sizeof(size_t));
            if (!tmp) {
                free(copy);
                free(sizes);
                return -1;
            }
            sizes = tmp;
            cap = new_cap;
        }

        sizes[count++] = (size_t)value;
        if ((size_t)value > max_size) {
            max_size = (size_t)value;
        }
    }

    free(copy);

    if (count == 0) {
        free(sizes);
        return -1;
    }

    *sizes_out = sizes;
    *count_out = count;
    *max_size_out = max_size;
    return 0;
}

static int parse_args(int argc, char **argv, bench_config_t *cfg)
{
    static const struct option long_opts[] = {
        {"server-cert", required_argument, NULL, 'c'},
        {"server-key", required_argument, NULL, 'k'},
        {"ca-cert", required_argument, NULL, 'a'},
        {"tls-version", required_argument, NULL, 't'},
        {"cipher", required_argument, NULL, 'C'},
        {"sizes", required_argument, NULL, 's'},
        {"iterations", required_argument, NULL, 'n'},
        {"warmup", required_argument, NULL, 'w'},
        {"core", required_argument, NULL, 'p'},
        {"summary-out", required_argument, NULL, 'S'},
        {"raw-out", required_argument, NULL, 'R'},
        {NULL, 0, NULL, 0}
    };

    int opt;

    memset(cfg, 0, sizeof(*cfg));
    cfg->iterations = DEFAULT_ITERATIONS;
    cfg->warmup = DEFAULT_WARMUP;
    cfg->sizes_csv = DEFAULT_SIZES;
    cfg->tls_version = DEFAULT_TLS_VERSION;
    cfg->core = -1;

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': cfg->server_cert = optarg; break;
            case 'k': cfg->server_key = optarg; break;
            case 'a': cfg->ca_cert = optarg; break;
            case 't': cfg->tls_version = optarg; break;
            case 'C': cfg->cipher = optarg; break;
            case 's': cfg->sizes_csv = optarg; break;
            case 'n': cfg->iterations = atoi(optarg); break;
            case 'w': cfg->warmup = atoi(optarg); break;
            case 'p': cfg->core = atoi(optarg); break;
            case 'S': cfg->summary_out = optarg; break;
            case 'R': cfg->raw_out = optarg; break;
            default:
                usage(argv[0]);
                return -1;
        }
    }

    if (!cfg->server_cert || !cfg->server_key || !cfg->ca_cert || !cfg->summary_out || !cfg->raw_out) {
        usage(argv[0]);
        return -1;
    }
    if (cfg->iterations <= 0 || cfg->warmup < 0) {
        fprintf(stderr, "iterations must be > 0 and warmup must be >= 0\n");
        return -1;
    }
    if (strcmp(cfg->tls_version, "1.2") != 0 && strcmp(cfg->tls_version, "1.3") != 0) {
        fprintf(stderr, "tls-version must be 1.2 or 1.3\n");
        return -1;
    }

    set_default_cipher(cfg);
    return 0;
}

int main(int argc, char **argv)
{
    bench_config_t cfg;
    size_t *sizes = NULL;
    size_t size_count = 0;
    size_t max_payload = 0;
    unsigned char *plaintext = NULL;
    unsigned char *recv_buf = NULL;
    FILE *summary_fp = NULL;
    FILE *raw_fp = NULL;
    bool wolfssl_initialized = false;
    size_t i;
    int exit_code = 1;

    if (parse_args(argc, argv, &cfg) != 0) {
        return 1;
    }

    if (pin_to_core(cfg.core) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    if (parse_sizes(cfg.sizes_csv, &sizes, &size_count, &max_payload) != 0) {
        fprintf(stderr, "failed to parse payload sizes: %s\n", cfg.sizes_csv);
        return 1;
    }

    plaintext = (unsigned char *)malloc(max_payload);
    recv_buf = (unsigned char *)malloc(max_payload);
    if (!plaintext || !recv_buf) {
        fprintf(stderr, "failed to allocate payload buffers\n");
        goto cleanup;
    }

    for (i = 0; i < max_payload; ++i) {
        plaintext[i] = (unsigned char)(i % 251U);
    }

    summary_fp = fopen(cfg.summary_out, "w");
    raw_fp = fopen(cfg.raw_out, "w");
    if (!summary_fp || !raw_fp) {
        perror("fopen");
        goto cleanup;
    }

    fprintf(summary_fp,
            "payload_size,iterations,warmup,min_ns,mean_ns,stddev_ns,p50_ns,p95_ns,p99_ns,max_ns,min_us,mean_us,p50_us,p95_us,p99_us,max_us,tls_version,cipher\n");
    fprintf(raw_fp,
            "payload_size,sample_index,delta_cycles,cntfrq,delta_ns,delta_us,tls_version,cipher\n");

    wolfSSL_Init();
    wolfssl_initialized = true;

    for (i = 0; i < size_count; ++i) {
        ssl_pair_t pair;
        uint64_t *samples = NULL;
        const char *tls_name;
        const char *cipher_name;
        stats_t stats;
        size_t j;

        memset(&pair, 0, sizeof(pair));
        samples = (uint64_t *)malloc((size_t)cfg.iterations * sizeof(uint64_t));
        if (!samples) {
            fprintf(stderr, "failed to allocate samples for payload %zu\n", sizes[i]);
            goto cleanup;
        }

        if (setup_ssl_pair(&pair, &cfg, sizes[i]) != 0) {
            free(samples);
            goto cleanup;
        }

        tls_name = wolfSSL_get_version(pair.client_ssl);
        cipher_name = wolfSSL_get_cipher_name(pair.client_ssl);
        if (!tls_name) {
            tls_name = cfg.tls_version;
        }
        if (!cipher_name) {
            cipher_name = cfg.cipher;
        }

        for (j = 0; j < (size_t)cfg.warmup; ++j) {
            if (run_one_iteration(&pair, plaintext, recv_buf, sizes[i], NULL, NULL, NULL) != 0) {
                free_ssl_pair(&pair);
                free(samples);
                goto cleanup;
            }
        }

        for (j = 0; j < (size_t)cfg.iterations; ++j) {
            uint64_t delta_cycles;
            uint64_t cntfrq;
            uint64_t delta_ns;

            if (run_one_iteration(&pair, plaintext, recv_buf, sizes[i], &delta_cycles, &cntfrq, &delta_ns) != 0) {
                free_ssl_pair(&pair);
                free(samples);
                goto cleanup;
            }

            samples[j] = delta_ns;
            fprintf(raw_fp,
                    "%zu,%zu,%llu,%llu,%llu,%.3f,%s,%s\n",
                    sizes[i],
                    j + 1,
                    (unsigned long long)delta_cycles,
                    (unsigned long long)cntfrq,
                    (unsigned long long)delta_ns,
                    (double)delta_ns / 1000.0,
                    tls_name,
                    cipher_name);
        }

        stats = compute_stats(samples, (size_t)cfg.iterations);
        fprintf(summary_fp,
                "%zu,%d,%d,%llu,%.3f,%.3f,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%s\n",
                sizes[i],
                cfg.iterations,
                cfg.warmup,
                (unsigned long long)stats.min_ns,
                stats.mean_ns,
                stats.stddev_ns,
                (unsigned long long)stats.p50_ns,
                (unsigned long long)stats.p95_ns,
                (unsigned long long)stats.p99_ns,
                (unsigned long long)stats.max_ns,
                (double)stats.min_ns / 1000.0,
                stats.mean_ns / 1000.0,
                (double)stats.p50_ns / 1000.0,
                (double)stats.p95_ns / 1000.0,
                (double)stats.p99_ns / 1000.0,
                (double)stats.max_ns / 1000.0,
                tls_name,
                cipher_name);

        printf("payload=%7zu B  min=%10.3f us  mean=%10.3f us  p99=%10.3f us  cipher=%s\n",
               sizes[i],
               (double)stats.min_ns / 1000.0,
               stats.mean_ns / 1000.0,
               (double)stats.p99_ns / 1000.0,
               cipher_name);

        free_ssl_pair(&pair);
        free(samples);
    }

    exit_code = 0;

cleanup:
    if (wolfssl_initialized) {
        wolfSSL_Cleanup();
    }
    if (summary_fp) {
        fclose(summary_fp);
    }
    if (raw_fp) {
        fclose(raw_fp);
    }
    free(plaintext);
    free(recv_buf);
    free(sizes);
    return exit_code;
}