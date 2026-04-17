#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define TLSMIGRATE_MAGIC 0x544D4754U /* 'TMGT' */
#define TLSMIGRATE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t t0_ns;
    tlspeek_serial_t serial;
} tlsmigrate_payload_t;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle) {
    const size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) {
        return -1;
    }

    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static long long parse_content_length(const char *headers, size_t headers_len) {
    if (!headers || headers_len == 0) {
        return -1;
    }

    // Ensure we can treat it like a string during parsing.
    // We expect the caller to have a NUL terminator in the backing buffer.
    const char *p = headers;
    const char *end = headers + headers_len;
    const char *needle = "content-length:";

    while (p < end) {
        // Find end of line
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) {
            line_end = end;
        } else {
            line_end++; // include '\n'
        }

        // Check if this line starts with Content-Length (case-insensitive)
        // Allow optional leading \r
        const char *line = p;
        while (line < line_end && (*line == '\r' || *line == '\n')) {
            line++;
        }

        if (line < line_end) {
            const char *found = strcasestr(line, needle);
            if (found == line) {
                const char *v = line + strlen(needle);
                while (v < line_end && (*v == ' ' || *v == '\t')) {
                    v++;
                }
                char *num_end = NULL;
                errno = 0;
                long long cl = strtoll(v, &num_end, 10);
                if (errno == 0 && num_end && num_end > v && cl >= 0) {
                    return cl;
                }
            }
        }

        p = line_end;
    }

    return -1;
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

static int handle_one_request(WOLFSSL_CTX *wctx, int client_fd, const tlsmigrate_payload_t *payload,
                              const char *expected_function_name) {
    int rc = -1;

    char request_owner[128];
    if (peek_owner_from_kernel(client_fd, &payload->serial,
                               request_owner, sizeof(request_owner)) != 0) {
        return -1;
    }
    if (expected_function_name && expected_function_name[0] != '\0' &&
        strcmp(request_owner, expected_function_name) != 0) {
        fprintf(stderr,
                "[timing-fn-worker] request owner mismatch: expected %s got %s\n",
                expected_function_name,
                request_owner);
        return -1;
    }

    fprintf(stderr,
            "[timing-fn-worker] owner verified via tls_read_peek: %s\n",
            request_owner);

    WOLFSSL *ssl = wolfSSL_new(wctx);
    if (!ssl) {
        return -1;
    }

    wolfSSL_set_fd(ssl, client_fd);
    if (tlspeek_restore(ssl, &payload->serial) != 0) {
        wolfSSL_free(ssl);
        return -1;
    }
    wolfSSL_set_fd(ssl, client_fd);

    unsigned char buf[65536 + 1];
    // After the stateless ownership check, consume the request for real.
    size_t total = 0;
    ssize_t header_end = -1;

    while (header_end < 0) {
        if (total >= sizeof(buf) - 1) {
            goto out;
        }

        int n = wolfSSL_read(ssl, buf + total, (int)(sizeof(buf) - 1 - total));
        if (n <= 0) {
            int err = wolfSSL_get_error(ssl, n);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            goto out;
        }
        total += (size_t)n;
        buf[total] = '\0';

        header_end = find_subseq(buf, total, "\r\n\r\n");
        if (header_end >= 0) {
            header_end += 4;
            break;
        }

        // Fallback: tolerate LF-only
        header_end = find_subseq(buf, total, "\n\n");
        if (header_end >= 0) {
            header_end += 2;
            break;
        }
    }

    const uint64_t header_ns = monotonic_ns();

    const size_t headers_len = (size_t)header_end;
    long long content_length = parse_content_length((const char *)buf, headers_len);
    if (content_length < 0) {
        content_length = 0;
    }

    size_t body_bytes_read = 0;
    size_t body_start = headers_len;
    if (total > body_start) {
        size_t already = total - body_start;
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
            goto out;
        }
        body_bytes_read += (size_t)n;
    }

    const uint64_t body_done_ns = monotonic_ns();

    const uint64_t t0_ns = payload->t0_ns;
    const uint64_t delta_header_ns = (t0_ns != 0) ? (header_ns - t0_ns) : 0;
    const uint64_t delta_body_ns = (t0_ns != 0) ? (body_done_ns - t0_ns) : 0;

    char json[512];
    int json_len = snprintf(
        json,
        sizeof(json),
        "{\"t0_ns\":%" PRIu64
        ",\"header_ns\":%" PRIu64
        ",\"body_done_ns\":%" PRIu64
        ",\"delta_header_ns\":%" PRIu64
        ",\"delta_body_ns\":%" PRIu64
        ",\"body_bytes_read\":%zu,\"content_length\":%lld}",
        t0_ns,
        header_ns,
        body_done_ns,
        delta_header_ns,
        delta_body_ns,
        body_bytes_read,
        content_length);
    if (json_len <= 0 || (size_t)json_len >= sizeof(json)) {
        goto out;
    }

    char resp[1024];
    int resp_len = snprintf(
        resp,
        sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        json_len,
        json);
    if (resp_len <= 0 || (size_t)resp_len >= sizeof(resp)) {
        goto out;
    }

    if (write_all_tls(ssl, resp, (size_t)resp_len) != 0) {
        goto out;
    }

    rc = 0;

out:
    wolfSSL_set_quiet_shutdown(ssl, 1);
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    return rc;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    const char *socket_dir = getenv("TLSMIGRATE_SOCKET_DIR");
    if (!socket_dir || socket_dir[0] == '\0') {
        socket_dir = "/run/tlsmigrate";
    }

    const char *function_name = getenv("TLSMIGRATE_FUNCTION_NAME");
    if (!function_name || function_name[0] == '\0') {
        function_name = "timing-fn";
    }

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, function_name);

    // Ensure permissive socket permissions for cross-container access.
    umask(0);

    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr,
                "[timing-fn-worker] warning: mkdir(%s) failed: %s\n",
                socket_dir,
                strerror(errno));
    } else {
        (void)chmod(socket_dir, 0777);
    }

    wolfSSL_Init();

    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) {
        fprintf(stderr, "[timing-fn-worker] wolfSSL_CTX_new failed\n");
        return 1;
    }

    // Best-effort load cert/key (not strictly required for import path)
    const char *cert_file = getenv("TLSMIGRATE_CERT");
    const char *key_file = getenv("TLSMIGRATE_KEY");
    if (!cert_file || cert_file[0] == '\0') cert_file = "/certs/server.crt";
    if (!key_file || key_file[0] == '\0') key_file = "/certs/server.key";

    if (wolfSSL_CTX_use_certificate_file(wctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[timing-fn-worker] warning: could not load cert %s\n", cert_file);
    }
    if (wolfSSL_CTX_use_PrivateKey_file(wctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[timing-fn-worker] warning: could not load key %s\n", key_file);
    }

    int listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    // Make sure the socket is writable by the gateway user inside its container.
    if (chmod(sock_path, 0777) != 0) {
        fprintf(stderr, "[timing-fn-worker] warning: chmod(%s) failed: %s\n", sock_path, strerror(errno));
    }

    fprintf(stderr, "[timing-fn-worker] listening on %s\n", sock_path);

    for (;;) {
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) {
            continue;
        }

        tlsmigrate_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        int client_fd = -1;

        if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
            close(conn_fd);
            continue;
        }
        close(conn_fd);

        if (client_fd < 0) {
            continue;
        }

        if (payload.magic != TLSMIGRATE_MAGIC || payload.version != TLSMIGRATE_VERSION) {
            fprintf(stderr,
                    "[timing-fn-worker] bad payload magic/version: 0x%08x/%u\n",
                    payload.magic,
                    payload.version);
            close(client_fd);
            continue;
        }

        (void)handle_one_request(wctx, client_fd, &payload, function_name);
        close(client_fd);
    }

    // Unreachable
    // close(listen_fd);
    // wolfSSL_CTX_free(wctx);
    // wolfSSL_Cleanup();
    // return 0;
}
