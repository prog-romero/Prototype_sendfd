#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <tlspeek/tlspeek.h>
#include <tlspeek/tlspeek_serial.h>
#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

#include "tlsmigrate_keepalive.h"

#define BENCH_HEADER_NAME "x-bench-t0-ns:"

typedef struct {
    uint64_t t0_ns;
    uint64_t header_ns;
    uint64_t body_done_ns;
    uint64_t delta_header_ns;
    uint64_t delta_body_ns;
    size_t body_bytes_read;
    long long content_length;
    int should_close;
} request_metrics_t;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} recv_buffer_t;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int ensure_capacity(recv_buffer_t *buf, size_t needed) {
    if (needed <= buf->cap) {
        return 0;
    }

    size_t new_cap = buf->cap ? buf->cap : 8192;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) {
            return -1;
        }
        new_cap *= 2;
    }

    unsigned char *new_data = realloc(buf->data, new_cap);
    if (!new_data) {
        return -1;
    }

    buf->data = new_data;
    buf->cap = new_cap;
    return 0;
}

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || len < needle_len) {
        return -1;
    }
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static long long parse_header_int64(const char *headers, size_t headers_len, const char *needle) {
    const char *p = headers;
    const char *end = headers + headers_len;
    size_t needle_len = strlen(needle);

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) {
            line_end = end;
        } else {
            line_end++;
        }

        const char *line = p;
        while (line < line_end && (*line == '\r' || *line == '\n')) {
            line++;
        }

        if ((size_t)(line_end - line) >= needle_len && strncasecmp(line, needle, needle_len) == 0) {
            const char *value = line + needle_len;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            errno = 0;
            char *num_end = NULL;
            long long parsed = strtoll(value, &num_end, 10);
            if (errno == 0 && num_end && num_end > value) {
                return parsed;
            }
        }

        p = line_end;
    }

    return -1;
}

static long long parse_content_length(const char *headers, size_t headers_len) {
    return parse_header_int64(headers, headers_len, "content-length:");
}

static int header_has_token(const char *headers, size_t headers_len,
                            const char *header_name, const char *token) {
    const char *p = headers;
    const char *end = headers + headers_len;
    size_t name_len = strlen(header_name);
    size_t token_len = strlen(token);

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) {
            line_end = end;
        } else {
            line_end++;
        }

        const char *line = p;
        while (line < line_end && (*line == '\r' || *line == '\n')) {
            line++;
        }

        size_t line_len = (size_t)(line_end - line);
        if (line_len >= name_len && strncasecmp(line, header_name, name_len) == 0) {
            const char *value = line + name_len;
            const char *value_end = line_end;
            while (value < value_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            for (const char *it = value; it + token_len <= value_end; it++) {
                if (strncasecmp(it, token, token_len) == 0) {
                    return 1;
                }
            }
        }

        p = line_end;
    }

    return 0;
}

static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *owner_out, size_t owner_out_sz) {
    if (!buf || len == 0 || !owner_out || owner_out_sz == 0) {
        return false;
    }

    size_t line_end = 0;
    while (line_end < len && buf[line_end] != '\n') {
        line_end++;
    }
    if (line_end == 0) {
        return false;
    }

    const unsigned char *first_space = memchr(buf, ' ', line_end);
    if (!first_space) {
        return false;
    }
    const unsigned char *path = first_space + 1;
    size_t path_len = line_end - (size_t)(path - buf);
    const unsigned char *second_space = memchr(path, ' ', path_len);
    if (!second_space) {
        return false;
    }

    static const char prefix[] = "/function/";
    size_t req_path_len = (size_t)(second_space - path);
    if (req_path_len <= strlen(prefix) || memcmp(path, prefix, strlen(prefix)) != 0) {
        return false;
    }

    const unsigned char *name = path + strlen(prefix);
    size_t name_len = req_path_len - strlen(prefix);
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            name_len = i;
            break;
        }
    }

    if (name_len == 0 || name_len + 1 > owner_out_sz) {
        return false;
    }

    memcpy(owner_out, name, name_len);
    owner_out[name_len] = '\0';
    return true;
}

static int peek_owner_from_kernel(int client_fd, const tlspeek_serial_t *serial,
                                  char *owner_out, size_t owner_out_sz) {
    tlspeek_ctx_t peek_ctx;
    if (tlspeek_restore_peek_ctx(&peek_ctx, client_fd, serial) != 0) {
        return -1;
    }

    unsigned char peek_buf[8192 + 1];
    int peeked = tls_read_peek(&peek_ctx, peek_buf, sizeof(peek_buf) - 1);
    if (peeked <= 0) {
        tlspeek_free(&peek_ctx);
        return -1;
    }
    peek_buf[peeked] = '\0';

    int rc = parse_request_owner(peek_buf, (size_t)peeked, owner_out, owner_out_sz) ? 0 : -1;
    tlspeek_free(&peek_ctx);
    return rc;
}

static int write_all_tls(WOLFSSL *ssl, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t total = 0;
    while (total < len) {
        int rc = wolfSSL_write(ssl, p + total, (int)(len - total));
        if (rc > 0) {
            total += (size_t)rc;
            continue;
        }
        int err = wolfSSL_get_error(ssl, rc);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int wolfssl_read_more(WOLFSSL *ssl, recv_buffer_t *buf) {
    if (ensure_capacity(buf, buf->len + 4096 + 1) != 0) {
        return -1;
    }

    for (;;) {
        int n = wolfSSL_read(ssl, buf->data + buf->len, (int)(buf->cap - buf->len - 1));
        if (n > 0) {
            buf->len += (size_t)n;
            buf->data[buf->len] = '\0';
            return 0;
        }
        int err = wolfSSL_get_error(ssl, n);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            continue;
        }
        return -1;
    }
}

static int read_one_request(WOLFSSL *ssl, request_metrics_t *metrics) {
    recv_buffer_t buf = {0};
    ssize_t header_end = -1;

    memset(metrics, 0, sizeof(*metrics));
    if (ensure_capacity(&buf, 65536 + 1) != 0) {
        return -1;
    }

    while (header_end < 0) {
        if (buf.len >= 1024 * 1024) {
            free(buf.data);
            return -1;
        }

        if (wolfssl_read_more(ssl, &buf) != 0) {
            free(buf.data);
            return -1;
        }

        header_end = find_subseq(buf.data, buf.len, "\r\n\r\n");
        if (header_end >= 0) {
            header_end += 4;
            break;
        }

        header_end = find_subseq(buf.data, buf.len, "\n\n");
        if (header_end >= 0) {
            header_end += 2;
            break;
        }
    }

    metrics->header_ns = monotonic_ns();
    size_t headers_len = (size_t)header_end;
    long long content_length = parse_content_length((const char *)buf.data, headers_len);
    if (content_length < 0) {
        content_length = 0;
    }

    long long t0_ns = parse_header_int64((const char *)buf.data, headers_len, BENCH_HEADER_NAME);
    if (t0_ns > 0) {
        metrics->t0_ns = (uint64_t)t0_ns;
    }
    metrics->content_length = content_length;
    metrics->should_close = header_has_token((const char *)buf.data, headers_len, "connection:", "close");

    size_t body_bytes_read = 0;
    if (buf.len > headers_len) {
        size_t already = buf.len - headers_len;
        if (already > (size_t)content_length) {
            already = (size_t)content_length;
        }
        body_bytes_read += already;
    }

    while (body_bytes_read < (size_t)content_length) {
        unsigned char scratch[16384];
        size_t want = (size_t)content_length - body_bytes_read;
        if (want > sizeof(scratch)) {
            want = sizeof(scratch);
        }

        int n = wolfSSL_read(ssl, scratch, (int)want);
        if (n <= 0) {
            int err = wolfSSL_get_error(ssl, n);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            free(buf.data);
            return -1;
        }
        body_bytes_read += (size_t)n;
    }

    metrics->body_bytes_read = body_bytes_read;
    metrics->body_done_ns = monotonic_ns();
    if (metrics->t0_ns > 0) {
        metrics->delta_header_ns = metrics->header_ns - metrics->t0_ns;
        metrics->delta_body_ns = metrics->body_done_ns - metrics->t0_ns;
    }

    free(buf.data);
    return 0;
}

static int send_response(WOLFSSL *ssl, const request_metrics_t *metrics, const char *worker_name) {
    char json[768];
    int json_len = snprintf(
        json,
        sizeof(json),
        "{\"worker_name\":\"%s\",\"t0_ns\":%" PRIu64
        ",\"header_ns\":%" PRIu64
        ",\"body_done_ns\":%" PRIu64
        ",\"delta_header_ns\":%" PRIu64
        ",\"delta_body_ns\":%" PRIu64
        ",\"body_bytes_read\":%zu,\"content_length\":%lld}\n",
        worker_name,
        metrics->t0_ns,
        metrics->header_ns,
        metrics->body_done_ns,
        metrics->delta_header_ns,
        metrics->delta_body_ns,
        metrics->body_bytes_read,
        metrics->content_length);
    if (json_len <= 0 || (size_t)json_len >= sizeof(json)) {
        return -1;
    }

    char resp[1024];
    int resp_len = snprintf(
        resp,
        sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        json_len,
        metrics->should_close ? "close" : "keep-alive",
        json);
    if (resp_len <= 0 || (size_t)resp_len >= sizeof(resp)) {
        return -1;
    }

    return write_all_tls(ssl, resp, (size_t)resp_len);
}

static int export_live_serial(WOLFSSL *ssl, tlspeek_serial_t *serial) {
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    int rc;
    int key_size;
    int iv_size;
    word64 peer_seq = 0;
    const unsigned char *key;
    const unsigned char *iv;

    if (!ssl || !serial) {
        return -1;
    }

    serial->magic = TLSPEEK_MAGIC;
    rc = wolfSSL_tls_export(ssl, serial->tls_blob, &blob_sz);
    if (rc <= 0) {
        return -1;
    }
    serial->blob_sz = blob_sz;

    memset(serial->client_write_key, 0, sizeof(serial->client_write_key));
    memset(serial->client_write_iv, 0, sizeof(serial->client_write_iv));

    key = wolfSSL_GetClientWriteKey(ssl);
    iv = wolfSSL_GetClientWriteIV(ssl);
    key_size = wolfSSL_GetKeySize(ssl);
    iv_size = wolfSSL_GetIVSize(ssl);
    if (!key || !iv || key_size <= 0 || iv_size <= 0) {
        return -1;
    }
    if ((size_t)key_size > sizeof(serial->client_write_key)) {
        key_size = (int)sizeof(serial->client_write_key);
    }
    if ((size_t)iv_size > sizeof(serial->client_write_iv)) {
        iv_size = (int)sizeof(serial->client_write_iv);
    }

    memcpy(serial->client_write_key, key, (size_t)key_size);
    memcpy(serial->client_write_iv, iv, (size_t)iv_size);

    rc = wolfSSL_GetPeerSequenceNumber(ssl, &peer_seq);
    if (rc < 0) {
        return -1;
    }
    serial->read_seq_num = (uint64_t)peer_seq;
    return 0;
}

static int relay_to_gateway(int client_fd, tlsmigrate_keepalive_payload_t *payload,
                            const char *relay_socket) {
    int relay_fd;

    relay_fd = unix_client_connect(relay_socket);
    if (relay_fd < 0) {
        close(client_fd);
        return -1;
    }

    if (sendfd_with_state(relay_fd, client_fd, payload, sizeof(*payload)) != 0) {
        close(relay_fd);
        close(client_fd);
        return -1;
    }

    close(relay_fd);
    return 0;
}

static int process_session(WOLFSSL_CTX *wctx, int client_fd,
                           const tlsmigrate_keepalive_payload_t *initial_payload,
                           const char *function_name,
                           const char *relay_socket) {
    int rc = -1;
    tlsmigrate_keepalive_payload_t state;
    WOLFSSL *ssl = NULL;

    memset(&state, 0, sizeof(state));
    memcpy(&state, initial_payload, sizeof(state));

    ssl = wolfSSL_new(wctx);
    if (!ssl) {
        close(client_fd);
        return -1;
    }

    wolfSSL_set_fd(ssl, client_fd);
    if (tlspeek_restore(ssl, &state.serial) != 0) {
        goto out;
    }
    wolfSSL_set_fd(ssl, client_fd);

    for (;;) {
        char request_owner[TLSMIGRATE_KEEPALIVE_TARGET_LEN];
        request_metrics_t metrics;

        if (peek_owner_from_kernel(client_fd, &state.serial,
                                   request_owner, sizeof(request_owner)) != 0) {
            rc = 0;
            goto out;
        }

        if (strcmp(request_owner, function_name) != 0) {
            memset(state.target_function, 0, sizeof(state.target_function));
            snprintf(state.target_function, sizeof(state.target_function), "%s", request_owner);
            if (relay_to_gateway(client_fd, &state, relay_socket) != 0) {
                rc = -1;
            } else {
                client_fd = -1;
                rc = 0;
            }
            goto out;
        }

        if (read_one_request(ssl, &metrics) != 0) {
            goto out;
        }

        if (send_response(ssl, &metrics, function_name) != 0) {
            goto out;
        }

        if (metrics.should_close) {
            rc = 0;
            goto out;
        }

        if (export_live_serial(ssl, &state.serial) != 0) {
            goto out;
        }
    }

out:
    if (ssl) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
    }
    if (client_fd >= 0) {
        close(client_fd);
    }
    return rc;
}

int main(void) {
    const char *socket_dir = getenv("TLSMIGRATE_SOCKET_DIR");
    const char *function_name = getenv("TLSMIGRATE_FUNCTION_NAME");
    const char *relay_socket = getenv("TLSMIGRATE_RELAY_SOCKET");
    const char *cert_file = getenv("TLSMIGRATE_CERT");
    const char *key_file = getenv("TLSMIGRATE_KEY");
    char sock_path[256];
    int listen_fd;
    WOLFSSL_CTX *wctx;

    signal(SIGPIPE, SIG_IGN);

    if (!socket_dir || socket_dir[0] == '\0') {
        socket_dir = "/run/tlsmigrate";
    }
    if (!function_name || function_name[0] == '\0') {
        function_name = "timing-ka-a";
    }
    if (!relay_socket || relay_socket[0] == '\0') {
        relay_socket = "/run/tlsmigrate/keepalive-relay.sock";
    }
    if (!cert_file || cert_file[0] == '\0') {
        cert_file = "/certs/server.crt";
    }
    if (!key_file || key_file[0] == '\0') {
        key_file = "/certs/server.key";
    }

    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, function_name);

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[timing-ka-worker] warning: mkdir(%s) failed: %s\n", socket_dir, strerror(errno));
    } else {
        (void)chmod(socket_dir, 0777);
    }

    wolfSSL_Init();
    wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) {
        fprintf(stderr, "[timing-ka-worker] wolfSSL_CTX_new failed\n");
        return 1;
    }

    if (wolfSSL_CTX_use_certificate_file(wctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[timing-ka-worker] warning: could not load cert %s\n", cert_file);
    }
    if (wolfSSL_CTX_use_PrivateKey_file(wctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[timing-ka-worker] warning: could not load key %s\n", key_file);
    }

    listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }
    if (chmod(sock_path, 0777) != 0) {
        fprintf(stderr, "[timing-ka-worker] warning: chmod(%s) failed: %s\n", sock_path, strerror(errno));
    }

    fprintf(stderr, "[timing-ka-worker] listening on %s, relay=%s\n", sock_path, relay_socket);

    for (;;) {
        tlsmigrate_keepalive_payload_t payload;
        int client_fd = -1;
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) {
            continue;
        }

        memset(&payload, 0, sizeof(payload));
        if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
            close(conn_fd);
            continue;
        }
        close(conn_fd);

        if (client_fd < 0) {
            continue;
        }
        if (payload.magic != TLSMIGRATE_KEEPALIVE_MAGIC || payload.version != TLSMIGRATE_KEEPALIVE_VERSION) {
            fprintf(stderr,
                    "[timing-ka-worker] bad payload magic/version: 0x%08x/%u\n",
                    payload.magic,
                    payload.version);
            close(client_fd);
            continue;
        }

        (void)process_session(wctx, client_fd, &payload, function_name, relay_socket);
    }
}