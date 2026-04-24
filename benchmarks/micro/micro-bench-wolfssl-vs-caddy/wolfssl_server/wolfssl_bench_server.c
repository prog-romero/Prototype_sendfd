#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <errno.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/bench2_rdtsc.h"

#define DEFAULT_LISTEN_HOST "127.0.0.1"
#define DEFAULT_LISTEN_PORT "9445"
#define DEFAULT_CERT_PATH "../../../../libtlspeek/certs/server.crt"
#define DEFAULT_KEY_PATH "../../../../libtlspeek/certs/server.key"
#define MAX_HEADER_BYTES (256U * 1024U)

typedef struct {
    const char *listen_host;
    const char *listen_port;
    const char *cert_path;
    const char *key_path;
    int core;
} bench_config_t;

static volatile sig_atomic_t g_stop = 0;
static uint64_t g_request_no = 0;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --listen-host HOST   Listen host (default: %s)\n"
        "  --listen-port PORT   Listen port (default: %s)\n"
        "  --cert PATH          Server certificate PEM\n"
        "  --key PATH           Server private key PEM\n"
        "  --core N             Pin process to CPU core N\n",
        prog,
        DEFAULT_LISTEN_HOST,
        DEFAULT_LISTEN_PORT);
}

static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }

    return 0;
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

static ssize_t find_double_crlf(const char *buf, size_t len)
{
    size_t i;

    if (len < 4) {
        return -1;
    }

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)i;
        }
    }

    return -1;
}

static long parse_content_length(const char *hdr, size_t hdr_len)
{
    const char *p = hdr;
    const char *end = hdr + hdr_len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char key[] = "content-length:";

        if (line_len >= sizeof(key) - 1) {
            bool match = true;
            size_t idx;

            for (idx = 0; idx < sizeof(key) - 1 && match; idx++) {
                char c = p[idx];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c - 'A' + 'a');
                }
                match = (c == key[idx]);
            }

            if (match) {
                const char *v = p + sizeof(key) - 1;

                while (v < end && (*v == ' ' || *v == '\t')) {
                    v++;
                }

                return strtol(v, NULL, 10);
            }
        }

        p = nl ? nl + 1 : end;
    }

    return -1;
}

static int ssl_write_all(WOLFSSL *ssl, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;

    while (len > 0) {
        int n = wolfSSL_write(ssl, p, (int)len);

        if (n <= 0) {
            int err = wolfSSL_get_error(ssl, n);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        }

        p += (size_t)n;
        len -= (size_t)n;
    }

    return 0;
}

static int ssl_accept_blocking(WOLFSSL *ssl)
{
    for (;;) {
        int ret = wolfSSL_accept(ssl);

        if (ret == SSL_SUCCESS) {
            return 0;
        }

        switch (wolfSSL_get_error(ssl, ret)) {
            case WOLFSSL_ERROR_WANT_READ:
            case WOLFSSL_ERROR_WANT_WRITE:
                continue;
            default:
                return -1;
        }
    }
}

static int ssl_read_blocking(WOLFSSL *ssl, void *buf, size_t len)
{
    for (;;) {
        int ret = wolfSSL_read(ssl, buf, (int)len);

        if (ret > 0) {
            return ret;
        }

        switch (wolfSSL_get_error(ssl, ret)) {
            case WOLFSSL_ERROR_WANT_READ:
            case WOLFSSL_ERROR_WANT_WRITE:
                continue;
            default:
                return -1;
        }
    }
}

static void log_wolfssl_error(const char *op, WOLFSSL *ssl, int ret)
{
    char err_buf[WOLFSSL_MAX_ERROR_SZ];
    int err = wolfSSL_get_error(ssl, ret);

    fprintf(stderr,
        "%s failed: ret=%d err=%d (%s)\n",
        op,
        ret,
        err,
        wolfSSL_ERR_error_string((unsigned long)err, err_buf));
}

static int create_listener(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int fd = -1;
    int one = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }

    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 128) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int load_server_ctx(WOLFSSL_CTX **out_ctx, const bench_config_t *cfg)
{
    WOLFSSL_CTX *ctx = NULL;

    ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx) {
        return -1;
    }

    if (wolfSSL_CTX_use_certificate_file(ctx, cfg->cert_path, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        return -1;
    }

    if (wolfSSL_CTX_use_PrivateKey_file(ctx, cfg->key_path, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        return -1;
    }

    *out_ctx = ctx;
    return 0;
}

static int send_json_response(WOLFSSL *ssl,
                              int status_code,
                              const char *status_text,
                              const char *json_body)
{
    char header[256];
    int header_len;
    size_t body_len = strlen(json_body);

    header_len = snprintf(header,
                          sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status_code,
                          status_text,
                          body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }

    if (ssl_write_all(ssl, header, (size_t)header_len) != 0) {
        return -1;
    }
    if (ssl_write_all(ssl, json_body, body_len) != 0) {
        return -1;
    }

    return 0;
}

static int handle_connection(int client_fd, WOLFSSL_CTX *ctx)
{
    WOLFSSL *ssl = NULL;
    char *req = NULL;
    size_t cap = 65536;
    size_t len = 0;
    ssize_t hdr_end = -1;
    size_t header_bytes;
    size_t body_in_buf;
    long content_length;
    uint64_t top1;
    uint64_t top2;
    uint64_t cntfrq;
    uint64_t request_no;
    const char *tls_version;
    const char *cipher_suite;
    int raw_fd;
    int ret = -1;

    ssl = wolfSSL_new(ctx);
    if (!ssl) {
        goto done;
    }

    wolfSSL_set_fd(ssl, client_fd);
    if (ssl_accept_blocking(ssl) != 0) {
        log_wolfssl_error("wolfSSL_accept", ssl, -1);
        goto done;
    }

    raw_fd = wolfSSL_get_fd(ssl);
    bench2_wait_readable(raw_fd);
    top1 = bench2_rdtsc();
    cntfrq = bench2_cntfrq();

    req = (char *)malloc(cap);
    if (!req) {
        goto done;
    }

    while (hdr_end < 0) {
        int n;

        if (len >= MAX_HEADER_BYTES) {
            (void)send_json_response(ssl, 431, "Request Header Fields Too Large",
                                     "{\"error\":\"headers too large\"}");
            goto done;
        }

        if (len + 4096 > cap) {
            size_t new_cap = cap * 2;
            char *new_req = (char *)realloc(req, new_cap);

            if (!new_req) {
                goto done;
            }
            req = new_req;
            cap = new_cap;
        }

        n = ssl_read_blocking(ssl, req + len, cap - len - 1);
        if (n <= 0) {
            goto done;
        }
        len += (size_t)n;
        hdr_end = find_double_crlf(req, len);
    }

    header_bytes = (size_t)hdr_end + 4;
    content_length = parse_content_length(req, (size_t)hdr_end);
    if (content_length < 0) {
        (void)send_json_response(ssl, 400, "Bad Request",
                                 "{\"error\":\"missing content-length\"}");
        goto done;
    }

    body_in_buf = (len > header_bytes) ? (len - header_bytes) : 0;
    while (body_in_buf < (size_t)content_length) {
        size_t want = (size_t)content_length - body_in_buf;
        int n;

        if (header_bytes + (size_t)content_length + 1 > cap) {
            size_t new_cap = header_bytes + (size_t)content_length + 1;
            char *new_req = (char *)realloc(req, new_cap);

            if (!new_req) {
                goto done;
            }
            req = new_req;
            cap = new_cap;
        }

        n = ssl_read_blocking(ssl, req + len, want);
        if (n <= 0) {
            goto done;
        }
        len += (size_t)n;
        body_in_buf += (size_t)n;
    }

    top2 = bench2_rdtsc();
    request_no = ++g_request_no;
    tls_version = wolfSSL_get_version(ssl);
    cipher_suite = wolfSSL_get_cipher(ssl);

    {
        char body[1024];
        int body_len = snprintf(body,
                                sizeof(body),
                                "{"
                                "\"implementation\":\"wolfssl\","
                                "\"request_no\":%llu,"
                                "\"top1_rdtsc\":%llu,"
                                "\"top2_rdtsc\":%llu,"
                                "\"delta_cycles\":%llu,"
                                "\"cntfrq\":%llu,"
                                "\"delta_ns\":%llu,"
                                "\"bytes_expected\":%ld,"
                                "\"bytes_consumed\":%zu,"
                                "\"tls_version\":\"%s\","
                                "\"cipher_suite\":\"%s\""
                                "}",
                                (unsigned long long)request_no,
                                (unsigned long long)top1,
                                (unsigned long long)top2,
                                (unsigned long long)(top2 - top1),
                                (unsigned long long)cntfrq,
                                (unsigned long long)(((top2 - top1) * 1000000000ULL) / cntfrq),
                                content_length,
                                body_in_buf,
                                tls_version ? tls_version : "unknown",
                                cipher_suite ? cipher_suite : "unknown");
        if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
            goto done;
        }
        if (send_json_response(ssl, 200, "OK", body) != 0) {
            goto done;
        }
    }

    ret = 0;

done:
    free(req);
    if (ssl) {
        (void)wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    close(client_fd);
    return ret;
}

static int parse_args(int argc, char **argv, bench_config_t *cfg)
{
    int i;

    cfg->listen_host = DEFAULT_LISTEN_HOST;
    cfg->listen_port = DEFAULT_LISTEN_PORT;
    cfg->cert_path = DEFAULT_CERT_PATH;
    cfg->key_path = DEFAULT_KEY_PATH;
    cfg->core = -1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen-host") == 0 && i + 1 < argc) {
            cfg->listen_host = argv[++i];
        }
        else if (strcmp(argv[i], "--listen-port") == 0 && i + 1 < argc) {
            cfg->listen_port = argv[++i];
        }
        else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            cfg->cert_path = argv[++i];
        }
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            cfg->key_path = argv[++i];
        }
        else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
            cfg->core = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 1;
        }
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    bench_config_t cfg;
    WOLFSSL_CTX *ctx = NULL;
    int listen_fd = -1;
    int parse_rc;

    parse_rc = parse_args(argc, argv, &cfg);
    if (parse_rc != 0) {
        return parse_rc > 0 ? 0 : 1;
    }

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        return 1;
    }

    if (pin_to_core(cfg.core) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    wolfSSL_Init();

    if (load_server_ctx(&ctx, &cfg) != 0) {
        fprintf(stderr, "failed to initialize wolfSSL server context\n");
        wolfSSL_Cleanup();
        return 1;
    }

    listen_fd = create_listener(cfg.listen_host, cfg.listen_port);
    if (listen_fd < 0) {
        perror("create_listener");
        wolfSSL_CTX_free(ctx);
        wolfSSL_Cleanup();
        return 1;
    }

    fprintf(stderr,
            "[wolfssl-bench] listening on %s:%s using cert=%s key=%s\n",
            cfg.listen_host,
            cfg.listen_port,
            cfg.cert_path,
            cfg.key_path);

    while (!g_stop) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                if (g_stop) {
                    break;
                }
                continue;
            }
            perror("accept");
            break;
        }

        if (handle_connection(client_fd, ctx) != 0) {
            fprintf(stderr, "[wolfssl-bench] request handling failed\n");
        }
    }

    close(listen_fd);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}