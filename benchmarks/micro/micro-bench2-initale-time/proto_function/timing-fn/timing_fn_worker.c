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

#include "bench2_rdtsc.h"

#define TLSMIGRATE_MAGIC 0x544D4754U /* 'TMGT' */
#define TLSMIGRATE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t top1_set;
    uint8_t _pad[7];
    tlspeek_serial_t serial;
} tlsmigrate_payload_t;

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

static int header_has_token(const unsigned char *hdr, size_t hdr_len,
                            const char *header_name, const char *token) {
    const char *p = (const char *)hdr;
    const char *end = p + hdr_len;
    size_t header_name_len = strlen(header_name);
    size_t token_len = strlen(token);

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? (nl + 1) : end;
        const char *line = p;

        while (line < line_end && (*line == '\r' || *line == '\n')) {
            line++;
        }

        if ((size_t)(line_end - line) >= header_name_len &&
            strncasecmp(line, header_name, header_name_len) == 0) {
            const char *value = line + header_name_len;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }

            for (const char *cursor = value; cursor + token_len <= line_end; cursor++) {
                if (strncasecmp(cursor, token, token_len) == 0) {
                    return 1;
                }
            }
        }

        p = line_end;
    }

    return 0;
}

static int ensure_capacity(unsigned char **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) {
        return 0;
    }

    size_t new_cap = *cap ? *cap : 8192;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) {
            return -1;
        }
        new_cap *= 2;
    }

    unsigned char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) {
        return -1;
    }

    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

static int read_more_tls(WOLFSSL *ssl, unsigned char **buf, size_t *len, size_t *cap) {
    if (ensure_capacity(buf, cap, *len + 4096 + 1) != 0) {
        return -1;
    }

    for (;;) {
        int n = wolfSSL_read(ssl, *buf + *len, (int)(*cap - *len - 1));
        if (n > 0) {
            *len += (size_t)n;
            (*buf)[*len] = '\0';
            return 0;
        }

        int err = wolfSSL_get_error(ssl, n);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            continue;
        }

        return -1;
    }
}

static ssize_t find_line_end(const unsigned char *buf, size_t start, size_t len, size_t *eol_len_out) {
    for (size_t i = start; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start && buf[i - 1] == '\r') {
                *eol_len_out = 2;
                return (ssize_t)(i - 1);
            }
            *eol_len_out = 1;
            return (ssize_t)i;
        }
    }

    return -1;
}

static int consume_chunked_body_tls(WOLFSSL *ssl, unsigned char **buf, size_t *len, size_t *cap,
                                    size_t cursor, size_t *body_bytes_read_out) {
    size_t body_bytes_read = 0;

    for (;;) {
        size_t eol_len = 0;
        ssize_t line_end = -1;
        while ((line_end = find_line_end(*buf, cursor, *len, &eol_len)) < 0) {
            if (read_more_tls(ssl, buf, len, cap) != 0) {
                return -1;
            }
        }

        size_t line_len = (size_t)line_end - cursor;
        if (line_len >= 64) {
            return -1;
        }

        char line[64];
        memcpy(line, *buf + cursor, line_len);
        line[line_len] = '\0';

        char *semi = strchr(line, ';');
        if (semi) {
            *semi = '\0';
        }

        errno = 0;
        char *num_end = NULL;
        unsigned long chunk_size = strtoul(line, &num_end, 16);
        if (errno != 0 || !num_end || num_end == line) {
            return -1;
        }

        cursor = (size_t)line_end + eol_len;

        if (chunk_size == 0) {
            for (;;) {
                while ((line_end = find_line_end(*buf, cursor, *len, &eol_len)) < 0) {
                    if (read_more_tls(ssl, buf, len, cap) != 0) {
                        return -1;
                    }
                }

                line_len = (size_t)line_end - cursor;
                cursor = (size_t)line_end + eol_len;
                if (line_len == 0) {
                    *body_bytes_read_out = body_bytes_read;
                    return 0;
                }
            }
        }

        while (*len - cursor < chunk_size + 1) {
            if (read_more_tls(ssl, buf, len, cap) != 0) {
                return -1;
            }
        }

        body_bytes_read += (size_t)chunk_size;
        cursor += (size_t)chunk_size;

        if (*len - cursor >= 2 && (*buf)[cursor] == '\r' && (*buf)[cursor + 1] == '\n') {
            cursor += 2;
        } else if (*len - cursor >= 1 && (*buf)[cursor] == '\n') {
            cursor += 1;
        } else {
            while (*len - cursor < 2) {
                if (read_more_tls(ssl, buf, len, cap) != 0) {
                    return -1;
                }
            }

            if ((*buf)[cursor] == '\r' && (*buf)[cursor + 1] == '\n') {
                cursor += 2;
            } else if ((*buf)[cursor] == '\n') {
                cursor += 1;
            } else {
                return -1;
            }
        }
    }
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

static int handle_one_request(WOLFSSL_CTX *wctx, int client_fd, const tlsmigrate_payload_t *payload) {
    int rc = -1;
    unsigned char *buf = NULL;
    size_t cap = 0;
    size_t total = 0;
    size_t body_bytes_read = 0;
    long long content_length = 0;

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

    ssize_t header_end = -1;
    size_t header_sep_len = 0;

    while (header_end < 0) {
        if (total >= 1024 * 1024) {
            goto out;
        }

        if (read_more_tls(ssl, &buf, &total, &cap) != 0) {
            goto out;
        }

        header_end = find_subseq(buf, total, "\r\n\r\n");
        if (header_end >= 0) {
            header_sep_len = 4;
            break;
        }

        header_end = find_subseq(buf, total, "\n\n");
        if (header_end >= 0) {
            header_sep_len = 2;
            break;
        }
    }

    const size_t headers_len = (size_t)header_end + header_sep_len;
    content_length = parse_content_length((const char *)buf, headers_len);
    if (content_length < 0) {
        content_length = 0;
    }

    if (header_has_token(buf, headers_len, "transfer-encoding:", "chunked")) {
        if (consume_chunked_body_tls(ssl, &buf, &total, &cap, headers_len, &body_bytes_read) != 0) {
            goto out;
        }
    } else {
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
    }

    const uint64_t top2 = bench2_rdtsc();
    free(buf);
    buf = NULL;
    const uint64_t freq = bench2_cntfrq();
    const uint64_t top1 = payload->top1_set ? payload->top1_rdtsc : 0;
    const uint64_t cntfrq = payload->cntfrq ? payload->cntfrq : freq;
    const uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;
    const uint64_t delta_ns =
        (cntfrq > 0) ? (uint64_t)(((long double)delta * 1000000000.0L) / (long double)cntfrq) : 0;

    char json[512];
    int json_len = snprintf(
        json,
        sizeof(json),
        "{\"path\":\"prototype\""
        ",\"top1_rdtsc\":%" PRIu64
        ",\"top2_rdtsc\":%" PRIu64
        ",\"delta_cycles\":%" PRIu64
        ",\"cntfrq\":%" PRIu64
        ",\"delta_ns\":%" PRIu64
        ",\"body_bytes_read\":%zu,\"content_length\":%lld}",
        top1,
        top2,
        delta,
        cntfrq,
        delta_ns,
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
    free(buf);
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

        (void)handle_one_request(wctx, client_fd, &payload);
        close(client_fd);
    }

    // Unreachable
    // close(listen_fd);
    // wolfSSL_CTX_free(wctx);
    // wolfSSL_Cleanup();
    // return 0;
}
