/*
 * bench2_vanilla_fn.c — Benchmark-2 keepalive vanilla container function.
 *
 * Plain HTTP/1.1 server (no TLS — the bench gateway handles TLS termination
 * on port 8444). This implementation is fully epoll-based and serves many
 * gateway TCP connections concurrently in a single event loop.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define WORKER_MAX_EVENTS 128
#define WORKER_MAX_FDS 65536
#define INITIAL_BUF_CAP 65536
#define MAX_REQ_BYTES (1024 * 1024)

enum session_state {
    S_READ_HEADERS = 0,
    S_READ_BODY,
    S_WRITE_RESP,
};

typedef struct session {
    int fd;
    enum session_state state;
    uint64_t req_no;
    char *buf;
    size_t cap;
    size_t len;
    size_t hdr_bytes;
    size_t body_target;
    size_t body_bytes_read;
    int is_chunked;
    int should_close;
    long long content_length;
    double operand_a;
    double operand_b;
    char resp[896];
    size_t resp_len;
    size_t resp_off;
} session_t;

static int s_epoll_fd = -1;
static session_t *s_sessions[WORKER_MAX_FDS];
static const char *s_worker_name = NULL;

static int header_has_token(const char *hdr, size_t hdr_len,
                            const char *header_name, const char *token)
{
    const char *p = hdr;
    const char *end = hdr + hdr_len;
    size_t header_name_len = strlen(header_name);
    size_t token_len = strlen(token);

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? (nl + 1) : end;
        const char *line = p;

        while (line < line_end && (*line == '\r' || *line == '\n')) line++;

        if ((size_t)(line_end - line) >= header_name_len &&
            strncasecmp(line, header_name, header_name_len) == 0) {
            const char *value = line + header_name_len;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            for (const char *cursor = value; cursor + token_len <= line_end; cursor++) {
                if (strncasecmp(cursor, token, token_len) == 0) return 1;
            }
        }

        p = line_end;
    }

    return 0;
}

static long long parse_header_int64(const char *hdr, size_t hdr_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    const char *p = hdr;
    const char *end = hdr + hdr_len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len >= needle_len) {
            int match = 1;
            for (size_t k = 0; k < needle_len && match; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                match = (c == needle[k]);
            }
            if (match) {
                const char *v = p + needle_len;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                char *endp = NULL;
                long long val = strtoll(v, &endp, 10);
                if (endp && endp > v) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }

    return -1;
}

static int ensure_capacity(char **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) return 0;

    size_t new_cap = *cap ? *cap : INITIAL_BUF_CAP;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) return -1;
        new_cap *= 2;
    }

    char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return -1;

    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

static ssize_t find_subseq(const char *buf, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || len < needle_len) return -1;

    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return (ssize_t)i;
    }

    return -1;
}

static ssize_t find_line_end(const char *buf, size_t start, size_t len, size_t *eol_len_out)
{
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

static int parse_query_double_param(const char *start, const char *end,
                                    const char *key, double *value_out)
{
    size_t key_len = strlen(key);
    const char *cursor = start;

    while (cursor < end) {
        const char *segment_end = memchr(cursor, '&', (size_t)(end - cursor));
        if (!segment_end) segment_end = end;

        if ((size_t)(segment_end - cursor) > key_len + 1 &&
            memcmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            char number_buf[64];
            size_t number_len = (size_t)(segment_end - cursor - key_len - 1);
            if (number_len == 0 || number_len >= sizeof(number_buf)) return -1;
            memcpy(number_buf, cursor + key_len + 1, number_len);
            number_buf[number_len] = '\0';

            char *endp = NULL;
            double value = strtod(number_buf, &endp);
            if (endp && *endp == '\0') {
                *value_out = value;
                return 1;
            }
            return -1;
        }

        cursor = segment_end;
        if (cursor < end && *cursor == '&') cursor++;
    }

    return 0;
}

static void parse_request_operands(const char *buf, size_t len,
                                   double *operand_a_out, double *operand_b_out)
{
    double operand_a = 10.0;
    double operand_b = 20.0;

    const char *line_end = memchr(buf, '\n', len);
    if (!line_end) line_end = buf + len;

    const char *space1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (space1) {
        const char *path = space1 + 1;
        const char *space2 = memchr(path, ' ', (size_t)(line_end - path));
        if (space2 && path < space2) {
            const char *query = memchr(path, '?', (size_t)(space2 - path));
            if (query && query + 1 < space2) {
                (void)parse_query_double_param(query + 1, space2, "a", &operand_a);
                (void)parse_query_double_param(query + 1, space2, "b", &operand_b);
            }
        }
    }

    *operand_a_out = operand_a;
    *operand_b_out = operand_b;
}

static const char *worker_operation_name(const char *worker_name)
{
    if (worker_name && (strstr(worker_name, "fn-b") || strstr(worker_name, "product"))) {
        return "product";
    }
    return "sum";
}

static int epoll_add(int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod(int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static void session_reset(session_t *s)
{
    s->state = S_READ_HEADERS;
    s->len = 0;
    s->hdr_bytes = 0;
    s->body_target = 0;
    s->body_bytes_read = 0;
    s->is_chunked = 0;
    s->should_close = 0;
    s->content_length = 0;
    s->operand_a = 10.0;
    s->operand_b = 20.0;
    s->resp_len = 0;
    s->resp_off = 0;
}

static void session_close(session_t *s)
{
    if (!s) return;
    if (s->fd >= 0 && s->fd < WORKER_MAX_FDS) {
        s_sessions[s->fd] = NULL;
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
        close(s->fd);
    }
    free(s->buf);
    free(s);
}

static session_t *session_new(int fd)
{
    session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = fd;
    s->cap = INITIAL_BUF_CAP;
    s->buf = malloc(s->cap);
    if (!s->buf) {
        free(s);
        return NULL;
    }

    session_reset(s);
    return s;
}

static int session_recv_more(session_t *s)
{
    if (ensure_capacity(&s->buf, &s->cap, s->len + 4096 + 1) != 0) return -1;

    for (;;) {
        ssize_t n = recv(s->fd, s->buf + s->len, s->cap - s->len - 1, MSG_DONTWAIT);
        if (n > 0) {
            s->len += (size_t)n;
            s->buf[s->len] = '\0';
            return 1;
        }
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    }
}

static int session_parse_headers(session_t *s)
{
    ssize_t hdr_end = find_subseq(s->buf, s->len, "\r\n\r\n");
    size_t hdr_sep_len = 0;
    if (hdr_end >= 0) {
        hdr_sep_len = 4;
    } else {
        hdr_end = find_subseq(s->buf, s->len, "\n\n");
        if (hdr_end >= 0) hdr_sep_len = 2;
    }

    if (hdr_end < 0) return 0;

    s->hdr_bytes = (size_t)hdr_end + hdr_sep_len;
    s->content_length = parse_header_int64(s->buf, s->hdr_bytes, "content-length:");
    if (s->content_length < 0) s->content_length = 0;

    parse_request_operands(s->buf, s->hdr_bytes, &s->operand_a, &s->operand_b);
    s->is_chunked = header_has_token(s->buf, s->hdr_bytes, "transfer-encoding:", "chunked");
    s->should_close = header_has_token(s->buf, s->hdr_bytes, "connection:", "close");
    s->body_target = (size_t)s->content_length;
    s->body_bytes_read = (s->len > s->hdr_bytes) ? (s->len - s->hdr_bytes) : 0;
    if (s->body_bytes_read > s->body_target) s->body_bytes_read = s->body_target;
    s->state = S_READ_BODY;
    return 1;
}

static int session_finish_chunked(session_t *s)
{
    size_t cursor = s->hdr_bytes;
    size_t body_bytes_read = 0;

    for (;;) {
        size_t eol_len = 0;
        ssize_t line_end = find_line_end(s->buf, cursor, s->len, &eol_len);
        if (line_end < 0) {
            int rc = session_recv_more(s);
            if (rc <= 0) return -1;
            continue;
        }

        size_t line_len = (size_t)line_end - cursor;
        if (line_len >= 64) return -1;

        char line[64];
        memcpy(line, s->buf + cursor, line_len);
        line[line_len] = '\0';

        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';

        errno = 0;
        char *num_end = NULL;
        unsigned long chunk_size = strtoul(line, &num_end, 16);
        if (errno != 0 || !num_end || num_end == line) return -1;

        cursor = (size_t)line_end + eol_len;
        if (chunk_size == 0) {
            for (;;) {
                line_end = find_line_end(s->buf, cursor, s->len, &eol_len);
                if (line_end < 0) {
                    int rc = session_recv_more(s);
                    if (rc <= 0) return -1;
                    continue;
                }
                line_len = (size_t)line_end - cursor;
                cursor = (size_t)line_end + eol_len;
                if (line_len == 0) {
                    s->body_bytes_read = body_bytes_read;
                    return 0;
                }
            }
        }

        while (s->len - cursor < chunk_size + 1) {
            int rc = session_recv_more(s);
            if (rc <= 0) return -1;
        }

        body_bytes_read += (size_t)chunk_size;
        cursor += (size_t)chunk_size;

        if (s->len - cursor >= 2 && s->buf[cursor] == '\r' && s->buf[cursor + 1] == '\n') {
            cursor += 2;
        } else if (s->len - cursor >= 1 && s->buf[cursor] == '\n') {
            cursor += 1;
        } else {
            while (s->len - cursor < 2) {
                int rc = session_recv_more(s);
                if (rc <= 0) return -1;
            }
            if (s->buf[cursor] == '\r' && s->buf[cursor + 1] == '\n') {
                cursor += 2;
            } else if (s->buf[cursor] == '\n') {
                cursor += 1;
            } else {
                return -1;
            }
        }
    }
}

static int session_build_response(session_t *s)
{
    const char *operation = worker_operation_name(s_worker_name);
    double result = (strcmp(operation, "product") == 0)
        ? (s->operand_a * s->operand_b)
        : (s->operand_a + s->operand_b);

    char json[640];
    int jlen = snprintf(json, sizeof(json),
        "{\"worker\":\"%s\",\"request_no\":%" PRIu64
        ",\"path\":\"vanilla\""
        ",\"operation\":\"%s\""
        ",\"a\":%.2f,\"b\":%.2f,\"result\":%.2f}\n",
        s_worker_name,
        s->req_no,
        operation,
        s->operand_a,
        s->operand_b,
        result);
    if (jlen <= 0 || (size_t)jlen >= sizeof(json)) return -1;

    int rlen = snprintf(s->resp, sizeof(s->resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        jlen,
        s->should_close ? "close" : "keep-alive",
        json);
    if (rlen <= 0 || (size_t)rlen >= sizeof(s->resp)) return -1;

    s->resp_len = (size_t)rlen;
    s->resp_off = 0;
    s->state = S_WRITE_RESP;
    return epoll_mod(s->fd, EPOLLOUT);
}

static int session_step(session_t *s, uint32_t revents)
{
    if (revents & (EPOLLERR | EPOLLHUP)) return -1;

    switch (s->state) {
        case S_READ_HEADERS:
            if ((revents & EPOLLIN) == 0) return 0;
            if (s->len >= MAX_REQ_BYTES) return -1;
            if (session_recv_more(s) <= 0) return -1;
            if (session_parse_headers(s) <= 0) return 0;
            if (!s->is_chunked && s->body_bytes_read >= s->body_target) {
                return session_build_response(s);
            }
            return 0;

        case S_READ_BODY:
            if ((revents & EPOLLIN) == 0) return 0;
            if (s->is_chunked) {
                if (session_finish_chunked(s) != 0) return -1;
                return session_build_response(s);
            }

            if (session_recv_more(s) <= 0) return -1;
            s->body_bytes_read = (s->len > s->hdr_bytes) ? (s->len - s->hdr_bytes) : 0;
            if (s->body_bytes_read > s->body_target) s->body_bytes_read = s->body_target;
            if (s->body_bytes_read < s->body_target) return 0;
            return session_build_response(s);

        case S_WRITE_RESP:
            if ((revents & EPOLLOUT) == 0) return 0;
            while (s->resp_off < s->resp_len) {
                ssize_t n = write(s->fd, s->resp + s->resp_off, s->resp_len - s->resp_off);
                if (n > 0) {
                    s->resp_off += (size_t)n;
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
                return -1;
            }

            if (s->should_close) return -1;
            session_reset(s);
            return epoll_mod(s->fd, EPOLLIN);
    }

    return -1;
}

static void handle_accept(int listen_fd)
{
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("[bench2-fn] accept4");
            break;
        }

        if (conn_fd >= WORKER_MAX_FDS) {
            close(conn_fd);
            continue;
        }

        session_t *s = session_new(conn_fd);
        if (!s) {
            close(conn_fd);
            continue;
        }

        s_sessions[conn_fd] = s;
        if (epoll_add(conn_fd, EPOLLIN) < 0) {
            perror("[bench2-fn] epoll_add conn_fd");
            session_close(s);
            continue;
        }
    }
}

int main(void)
{
    const char *worker_name = getenv("BENCH2_WORKER_NAME");
    if (!worker_name || worker_name[0] == '\0') worker_name = "bench2-fn-?";

    const char *port_str = getenv("BENCH2_LISTEN_PORT");
    if (!port_str || port_str[0] == '\0') port_str = "8080";

    signal(SIGPIPE, SIG_IGN);

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;
    if (getaddrinfo(NULL, port_str, &hints, &res) != 0) {
        fprintf(stderr, "[bench2-fn] getaddrinfo failed\n");
        return 1;
    }

    int listen_fd = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        listen_fd = socket(it->ai_family, it->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, it->ai_protocol);
        if (listen_fd < 0) continue;

        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 && listen(listen_fd, 4096) == 0) {
            break;
        }

        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "[bench2-fn] bind/listen on port %s failed\n", port_str);
        return 1;
    }

    s_worker_name = worker_name;
    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) {
        perror("[bench2-fn] epoll_create1");
        close(listen_fd);
        return 1;
    }

    if (epoll_add(listen_fd, EPOLLIN) < 0) {
        perror("[bench2-fn] epoll_add listen_fd");
        close(s_epoll_fd);
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "[bench2-fn] %s listening on port %s [epoll multi-session]\n", worker_name, port_str);

    struct epoll_event events[WORKER_MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(s_epoll_fd, events, WORKER_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[bench2-fn] epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            if (fd == listen_fd) {
                handle_accept(listen_fd);
                continue;
            }

            if (fd < 0 || fd >= WORKER_MAX_FDS) continue;
            session_t *s = s_sessions[fd];
            if (!s) {
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }

            if (session_step(s, revents) != 0) {
                session_close(s);
            }
        }
    }

    close(s_epoll_fd);
    close(listen_fd);
    return 0;
}