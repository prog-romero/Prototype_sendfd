#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BENCH_HEADER_NAME "x-bench-t0-ns:"

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

static int read_more(int fd, recv_buffer_t *buf) {
    if (ensure_capacity(buf, buf->len + 4096 + 1) != 0) {
        return -1;
    }

    ssize_t n = recv(fd, buf->data + buf->len, buf->cap - buf->len - 1, 0);
    if (n <= 0) {
        return -1;
    }

    buf->len += (size_t)n;
    buf->data[buf->len] = '\0';
    return 0;
}

static int consume_chunked_body(int fd, recv_buffer_t *buf, size_t cursor, size_t *body_bytes_read_out) {
    size_t body_bytes_read = 0;

    for (;;) {
        size_t eol_len = 0;
        ssize_t line_end = -1;
        while ((line_end = find_line_end(buf->data, cursor, buf->len, &eol_len)) < 0) {
            if (read_more(fd, buf) != 0) {
                return -1;
            }
        }

        size_t line_len = (size_t)line_end - cursor;
        if (line_len >= 64) {
            return -1;
        }

        char line[64];
        memcpy(line, buf->data + cursor, line_len);
        line[line_len] = '\0';

        errno = 0;
        char *semi = strchr(line, ';');
        if (semi) {
            *semi = '\0';
        }
        char *num_end = NULL;
        unsigned long chunk_size = strtoul(line, &num_end, 16);
        if (errno != 0 || !num_end || num_end == line) {
            return -1;
        }

        cursor = (size_t)line_end + eol_len;

        if (chunk_size == 0) {
            for (;;) {
                while ((line_end = find_line_end(buf->data, cursor, buf->len, &eol_len)) < 0) {
                    if (read_more(fd, buf) != 0) {
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

        while (buf->len - cursor < chunk_size + 1) {
            if (read_more(fd, buf) != 0) {
                return -1;
            }
        }

        body_bytes_read += (size_t)chunk_size;
        cursor += (size_t)chunk_size;

        if (buf->len - cursor >= 2 && buf->data[cursor] == '\r' && buf->data[cursor + 1] == '\n') {
            cursor += 2;
        } else if (buf->len - cursor >= 1 && buf->data[cursor] == '\n') {
            cursor += 1;
        } else {
            while (buf->len - cursor < 2) {
                if (read_more(fd, buf) != 0) {
                    return -1;
                }
            }
            if (buf->data[cursor] == '\r' && buf->data[cursor + 1] == '\n') {
                cursor += 2;
            } else if (buf->data[cursor] == '\n') {
                cursor += 1;
            } else {
                return -1;
            }
        }
    }
}

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int handle_client(int client_fd) {
    recv_buffer_t buf = {0};
    if (ensure_capacity(&buf, 65536 + 1) != 0) {
        return -1;
    }

    ssize_t header_end = -1;

    while (header_end < 0) {
        if (buf.len >= 1024 * 1024) {
            return -1;
        }

        if (read_more(client_fd, &buf) != 0) {
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

    uint64_t header_ns = monotonic_ns();
    size_t headers_len = (size_t)header_end;
    long long content_length = parse_content_length((const char *)buf.data, headers_len);
    if (content_length < 0) {
        content_length = 0;
    }

    int is_chunked = header_has_token((const char *)buf.data, headers_len,
                                      "transfer-encoding:", "chunked");

    long long t0_ns = parse_header_int64((const char *)buf.data, headers_len, BENCH_HEADER_NAME);
    if (t0_ns < 0) {
        t0_ns = 0;
    }

    size_t body_bytes_read = 0;
    if (is_chunked) {
        if (consume_chunked_body(client_fd, &buf, headers_len, &body_bytes_read) != 0) {
            free(buf.data);
            return -1;
        }
    } else {
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
            ssize_t n = recv(client_fd, scratch, want, 0);
            if (n <= 0) {
                free(buf.data);
                return -1;
            }
            body_bytes_read += (size_t)n;
        }
    }

    uint64_t body_done_ns = monotonic_ns();
    uint64_t delta_header_ns = (t0_ns > 0) ? (header_ns - (uint64_t)t0_ns) : 0;
    uint64_t delta_body_ns = (t0_ns > 0) ? (body_done_ns - (uint64_t)t0_ns) : 0;

    char json[512];
    int json_len = snprintf(
        json,
        sizeof(json),
        "{\"t0_ns\":%" PRIu64
        ",\"header_ns\":%" PRIu64
        ",\"body_done_ns\":%" PRIu64
        ",\"delta_header_ns\":%" PRIu64
        ",\"delta_body_ns\":%" PRIu64
        ",\"body_bytes_read\":%zu,\"content_length\":%lld}\n",
        (uint64_t)t0_ns,
        header_ns,
        body_done_ns,
        delta_header_ns,
        delta_body_ns,
        body_bytes_read,
        content_length);
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
        "Connection: close\r\n"
        "\r\n"
        "%s",
        json_len,
        json);
    if (resp_len <= 0 || (size_t)resp_len >= sizeof(resp)) {
        return -1;
    }

    int rc = write_all(client_fd, resp, (size_t)resp_len);
    free(buf.data);
    return rc;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 128) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "timing_fn listening on :8080\n");

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            continue;
        }
        (void)handle_client(client_fd);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}