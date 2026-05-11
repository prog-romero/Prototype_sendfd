/*
 * bench2_proto_worker.c — Benchmark-2 keepalive prototype container worker.
 *
 * Each worker restores the TLS session exported by the gateway, serves
 * keep-alive requests for the function it owns, and relays the live fd back
 * through the gateway relay socket when ownership changes.
 *
 * Environment variables:
 *   BENCH2_FUNCTION_NAME    function name (e.g. "bench2-fn-a")
 *   BENCH2_SOCKET_DIR       directory for per-function sockets (default /run/bench2)
 *   BENCH2_RELAY_SOCKET     gateway relay socket path
 *   BENCH2_CERT             TLS certificate (default /certs/server.crt)
 *   BENCH2_KEY              TLS private key  (default /certs/server.key)
 * ──────────────────────────────────────────────────────────────────────────
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef HAVE_SECRET_CALLBACK
#  define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#  define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <tlspeek/tlspeek.h>
#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

#include "bench2_rdtsc.h"
#include "bench2_payload.h"

#ifdef BENCH2_WORKER_ENABLE_TRACE
#define BENCH2_WORKER_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define BENCH2_WORKER_TRACE(...) ((void)0)
#endif

/* ─── HTTP parsing helpers ──────────────────────────────────────────────── */

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0)
            return (ssize_t)i;
    return -1;
}

static long long parse_header_int64(const char *hdr, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p = hdr, *end = hdr + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll >= nlen) {
            int m = 1;
            for (size_t k = 0; k < nlen && m; k++) {
                char c = p[k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == needle[k]);
            }
            if (m) {
                const char *v = p + nlen;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                char *ep = NULL;
                long long val = strtoll(v, &ep, 10);
                if (ep && ep > v) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return -1;
}

static long long parse_content_length(const char *hdr, size_t hlen)
{
    return parse_header_int64(hdr, hlen, "content-length:");
}

static int has_connection_close(const char *hdr, size_t hlen)
{
    const char *p = hdr, *end = hdr + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char key[] = "connection:";
        size_t kl = sizeof(key) - 1;
        if (ll >= kl) {
            int m = 1;
            for (size_t k = 0; k < kl && m; k++) {
                char c = p[k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == key[k]);
            }
            if (m) {
                const char *v = p + kl, *ve = nl ? nl : end;
                while (v + 4 < ve) {
                    if ((v[0]=='c'||v[0]=='C') && (v[1]=='l'||v[1]=='L') &&
                        (v[2]=='o'||v[2]=='O') && (v[3]=='s'||v[3]=='S') &&
                        (v[4]=='e'||v[4]=='E')) return 1;
                    v++;
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    return 0;
}

static int has_single_owner_hint(const char *hdr, size_t hlen)
{
    return parse_header_int64(hdr, hlen, "x-bench2-single-owner:") > 0;
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

static void parse_request_operands(const unsigned char *buf, size_t len,
                                   double *operand_a_out, double *operand_b_out)
{
    double operand_a = 10.0;
    double operand_b = 20.0;

    const unsigned char *line_end = memchr(buf, '\n', len);
    if (!line_end) line_end = buf + len;

    const unsigned char *space1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (space1) {
        const unsigned char *path = space1 + 1;
        const unsigned char *space2 = memchr(path, ' ', (size_t)(line_end - path));
        if (space2 && path < space2) {
            const unsigned char *query = memchr(path, '?', (size_t)(space2 - path));
            if (query && query + 1 < space2) {
                const char *query_start = (const char *)(query + 1);
                const char *query_end = (const char *)space2;
                (void)parse_query_double_param(query_start, query_end, "a", &operand_a);
                (void)parse_query_double_param(query_start, query_end, "b", &operand_b);
            }
        }
    }

    *operand_a_out = operand_a;
    *operand_b_out = operand_b;
}

static const char *worker_operation_name(const char *function_name)
{
    if (function_name && (strstr(function_name, "fn-b") || strstr(function_name, "product"))) {
        return "product";
    }
    return "sum";
}

/* Parse function name from HTTP request line "METHOD /function/<name> HTTP..." */
static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *out, size_t outsz)
{
    if (!buf || !len || !out || !outsz) return false;
    size_t eol = 0;
    while (eol < len && buf[eol] != '\n') eol++;
    const unsigned char *sp1 = memchr(buf, ' ', eol);
    if (!sp1) return false;
    const unsigned char *path = sp1 + 1;
    static const char prefix[] = "/function/";
    size_t plen = eol - (size_t)(path - buf);
    const unsigned char *sp2 = memchr(path, ' ', plen);
    if (!sp2) return false;
    size_t rlen = (size_t)(sp2 - path);
    if (rlen <= sizeof(prefix) - 1 ||
        memcmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++)
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') { nlen = i; break; }
    if (!nlen || nlen + 1 > outsz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

static int peek_owner_from_kernel(int client_fd, const tlspeek_serial_t *serial,
                                  char *owner_out, size_t owner_out_sz)
{
    unsigned char peek_buf[8193];
    size_t want = sizeof(peek_buf) - 1;
    if (want > 1024) want = 1024;

    while (want > 0) {
        tlspeek_ctx_t peek_ctx;
        if (tlspeek_restore_peek_ctx(&peek_ctx, client_fd, serial) != 0) {
            return -1;
        }

        int peeked = tls_read_peek(&peek_ctx, peek_buf, want);
        tlspeek_free(&peek_ctx);
        if (peeked <= 0) {
            return -1;
        }

        peek_buf[peeked] = '\0';
        if (parse_request_owner(peek_buf, (size_t)peeked, owner_out, owner_out_sz)) {
            return 0;
        }

        if (want >= sizeof(peek_buf) - 1) break;
        want *= 2;
        if (want > sizeof(peek_buf) - 1) want = sizeof(peek_buf) - 1;
    }

    return -1;
}

/* ─── TLS helpers ───────────────────────────────────────────────────────── */

static void set_serial_cipher_from_name(tlspeek_serial_t *serial, const char *name)
{
    if (!serial) return;
    if (!name) {
        serial->cipher_suite = TLSPEEK_AES_256_GCM;
        return;
    }

    if (strstr(name, "CHACHA20"))
        serial->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES-128"))
        serial->cipher_suite = TLSPEEK_AES_128_GCM;
    else
        serial->cipher_suite = TLSPEEK_AES_256_GCM;
}

/* Export current live wolfSSL session key material into a serial struct.
 *
 * tls_blob (full session export) is needed for relay to another container.
 * tlspeek_restore_peek_ctx only needs client_write_key/iv + read_seq_num +
 * cipher_suite, so a failed wolfSSL_tls_export is non-fatal: self-peek still
 * works; only cross-container relay would be broken in that case.
 */
static int export_live_serial(WOLFSSL *ssl, tlspeek_serial_t *serial)
{
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    const unsigned char *key, *iv;
    int key_sz, iv_sz;
    word64 peer_seq = 0;

    if (!ssl || !serial) return -1;
    serial->magic = TLSPEEK_MAGIC;
    set_serial_cipher_from_name(serial, wolfSSL_get_cipher_name(ssl));

    /* Non-fatal: blob is only required for relay, not for local peek. */
    int rc = wolfSSL_tls_export(ssl, serial->tls_blob, &blob_sz);
    if (rc > 0)
        serial->blob_sz = blob_sz;
    else
        serial->blob_sz = 0; /* relay won't work but local peek still will */

    key    = wolfSSL_GetClientWriteKey(ssl);
    iv     = wolfSSL_GetClientWriteIV(ssl);
    key_sz = wolfSSL_GetKeySize(ssl);
    iv_sz  = wolfSSL_GetIVSize(ssl);
    if (!key || !iv || key_sz <= 0 || iv_sz <= 0) return -1;

    size_t ks = (size_t)key_sz < sizeof(serial->client_write_key)
                ? (size_t)key_sz : sizeof(serial->client_write_key);
    size_t is = (size_t)iv_sz  < sizeof(serial->client_write_iv)
                ? (size_t)iv_sz  : sizeof(serial->client_write_iv);
    memcpy(serial->client_write_key, key, ks);
    memcpy(serial->client_write_iv,  iv,  is);

    if (wolfSSL_GetPeerSequenceNumber(ssl, &peer_seq) < 0) return -1;
    serial->read_seq_num = (uint64_t)peer_seq;
    BENCH2_WORKER_TRACE("[bench2-worker] export_live_serial cipher=0x%04x name=%s seq=%" PRIu64 "\n",
                        (unsigned int)serial->cipher_suite,
                        wolfSSL_get_cipher_name(ssl) ? wolfSSL_get_cipher_name(ssl) : "<null>",
                        serial->read_seq_num);
    return 0;
}

/* ─── Send session back to relay socket ─────────────────────────────────── */

static int relay_to_gateway(int client_fd,
                            bench2_keepalive_payload_t *payload,
                            const char *relay_socket)
{
    int relay_fd = unix_client_connect(relay_socket);
    if (relay_fd < 0) { close(client_fd); return -1; }

    int rc = sendfd_with_state(relay_fd, client_fd, payload, sizeof(*payload));
    close(relay_fd);
    if (rc != 0) { close(client_fd); return -1; }
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

/* ─── Epoll-driven worker state ─────────────────────────────────────────── */

#define WORKER_MAX_EVENTS 128
#define WORKER_MAX_FDS 65536
#define WORKER_INITIAL_BUF 65536
#define WORKER_MAX_REQ_BYTES (1024 * 1024)

typedef enum {
    WS_PEEK_OWNER = 0,
    WS_READ_HEADERS,
    WS_READ_BODY,
    WS_WRITE_RESPONSE,
} worker_state_t;

typedef struct worker_session {
    int fd;
    WOLFSSL *ssl;
    bench2_keepalive_payload_t payload;
    worker_state_t state;
    uint64_t req_no;
    bool single_owner_direct_read;
    unsigned char *buf;
    size_t cap;
    size_t len;
    size_t hdr_sz;
    size_t body_in;
    size_t body_target;
    int should_close;
    int single_owner_hint;
    double operand_a;
    double operand_b;
    char resp[768];
    size_t resp_len;
    size_t resp_off;
} worker_session_t;

typedef struct handoff_conn {
    int fd;
    int client_fd;
    size_t len;
    bench2_keepalive_payload_t payload;
} handoff_conn_t;

static int s_epoll_fd = -1;
static WOLFSSL_CTX *s_wctx = NULL;
static const char *s_function_name = NULL;
static const char *s_relay_socket = NULL;
static size_t s_response_pad_bytes = 114;
static worker_session_t *s_sessions[WORKER_MAX_FDS];
static handoff_conn_t *s_handoffs[WORKER_MAX_FDS];

static uint32_t epoll_read_events(void)
{
    uint32_t events = EPOLLIN;
#ifdef EPOLLRDHUP
    events |= EPOLLRDHUP;
#endif
    return events;
}

static uint32_t epoll_write_events(void)
{
    uint32_t events = EPOLLOUT;
#ifdef EPOLLRDHUP
    events |= EPOLLRDHUP;
#endif
    return events;
}

static int epoll_add_fd(int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod_fd(int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static int ensure_capacity(unsigned char **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) return 0;
    size_t new_cap = *cap ? *cap : WORKER_INITIAL_BUF;
    while (new_cap < needed) {
        if (new_cap > ((size_t)-1) / 2) return -1;
        new_cap *= 2;
    }
    unsigned char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return -1;
    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

static int handoff_recv_nb(handoff_conn_t *h)
{
    if (!h || h->fd < 0) return -1;
    if (h->len >= sizeof(h->payload))
        return h->client_fd >= 0 ? 1 : -1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    unsigned char *payload_bytes = (unsigned char *)&h->payload;
    struct iovec iov = {
        .iov_base = payload_bytes + h->len,
        .iov_len = sizeof(h->payload) - h->len,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t rcvd;
    do {
        rcvd = recvmsg(h->fd, &msg, MSG_DONTWAIT);
    } while (rcvd < 0 && errno == EINTR);

    if (rcvd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (rcvd == 0 || (msg.msg_flags & MSG_CTRUNC))
        return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg) {
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
            return -1;

        int received_fd = -1;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
        if (h->client_fd >= 0)
            close(received_fd);
        else
            h->client_fd = received_fd;
    }

    h->len += (size_t)rcvd;
    if (h->len < sizeof(h->payload)) return 0;
    return h->client_fd >= 0 ? 1 : -1;
}

static int tls_record_ready(int fd)
{
    unsigned char hdr[TLSPEEK_HEADER_SIZE];
    ssize_t n;
    do {
        n = recv(fd, hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n == 0) return -1;
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if ((size_t)n < sizeof(hdr)) return 0;

    uint16_t record_len = (uint16_t)((hdr[3] << 8) | hdr[4]);
    if (record_len > TLSPEEK_MAX_RECORD + TLSPEEK_TAG_SIZE) return -1;

    int avail = 0;
    if (ioctl(fd, FIONREAD, &avail) != 0) return 0;
    return ((size_t)avail >= TLSPEEK_HEADER_SIZE + (size_t)record_len) ? 1 : 0;
}

static int wolfssl_want(worker_session_t *s, int err)
{
    if (err == WOLFSSL_ERROR_WANT_READ) {
        return epoll_mod_fd(s->fd, epoll_read_events());
    }
    if (err == WOLFSSL_ERROR_WANT_WRITE) {
        return epoll_mod_fd(s->fd, epoll_write_events());
    }
    return -1;
}

static void session_reset_request(worker_session_t *s, worker_state_t next_state)
{
    s->state = next_state;
    s->len = 0;
    s->hdr_sz = 0;
    s->body_in = 0;
    s->body_target = 0;
    s->should_close = 0;
    s->single_owner_hint = 0;
    s->operand_a = 10.0;
    s->operand_b = 20.0;
    s->resp_len = 0;
    s->resp_off = 0;
}

static void session_close(worker_session_t *s)
{
    if (!s) return;
    int fd = s->fd;
    if (fd >= 0 && fd < WORKER_MAX_FDS) {
        s_sessions[fd] = NULL;
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
    if (s->ssl) {
        wolfSSL_set_quiet_shutdown(s->ssl, 1);
        wolfSSL_free(s->ssl);
    }
    if (fd >= 0) close(fd);
    free(s->buf);
    free(s);
}

static worker_session_t *session_new(int client_fd,
                                     const bench2_keepalive_payload_t *payload)
{
    if (client_fd < 0 || client_fd >= WORKER_MAX_FDS) {
        close(client_fd);
        return NULL;
    }
    if (set_nonblocking(client_fd) != 0) {
        close(client_fd);
        return NULL;
    }

    worker_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        close(client_fd);
        return NULL;
    }

    s->fd = client_fd;
    s->ssl = wolfSSL_new(s_wctx);
    if (!s->ssl) {
        free(s);
        close(client_fd);
        return NULL;
    }
    wolfSSL_set_fd(s->ssl, client_fd);
    wolfSSL_set_using_nonblock(s->ssl, 1);
    memcpy(&s->payload, payload, sizeof(s->payload));
    if (tlspeek_restore(s->ssl, &s->payload.serial) != 0) {
        session_close(s);
        return NULL;
    }
    wolfSSL_set_fd(s->ssl, client_fd);
    wolfSSL_set_using_nonblock(s->ssl, 1);

    s->cap = WORKER_INITIAL_BUF;
    s->buf = malloc(s->cap);
    if (!s->buf) {
        session_close(s);
        return NULL;
    }

    s->req_no = 1;
    session_reset_request(s, WS_READ_HEADERS);
    s_sessions[client_fd] = s;

    BENCH2_WORKER_TRACE("[bench2-worker] registered fd=%d fn=%s target=%s seq=%" PRIu64 "\n",
                        client_fd,
                        s_function_name,
                        s->payload.target_function[0] ? s->payload.target_function : "<unset>",
                        s->payload.serial.read_seq_num);
    return s;
}

static int session_tls_read_buffer(worker_session_t *s)
{
    if (s->len >= WORKER_MAX_REQ_BYTES) return -1;
    if (ensure_capacity(&s->buf, &s->cap, s->len + 8192 + 1) != 0) return -1;

    size_t room = s->cap - s->len - 1;
    if (room > 8192) room = 8192;

    int n = wolfSSL_read(s->ssl, s->buf + s->len, (int)room);
    if (n > 0) {
        s->len += (size_t)n;
        s->buf[s->len] = '\0';
        return 1;
    }

    int err = wolfSSL_get_error(s->ssl, n);
    return wolfssl_want(s, err);
}

static int session_tls_drain_body(worker_session_t *s)
{
    while (s->body_in < s->body_target) {
        unsigned char scratch[8192];
        size_t want = s->body_target - s->body_in;
        if (want > sizeof(scratch)) want = sizeof(scratch);

        int n = wolfSSL_read(s->ssl, scratch, (int)want);
        if (n > 0) {
            s->body_in += (size_t)n;
            continue;
        }

        int err = wolfSSL_get_error(s->ssl, n);
        return wolfssl_want(s, err);
    }
    return 1;
}

static int session_build_response(worker_session_t *s)
{
    const char *operation = worker_operation_name(s_function_name);
    double result = (strcmp(operation, "product") == 0)
        ? (s->operand_a * s->operand_b)
        : (s->operand_a + s->operand_b);

    char json[512];
    int jlen = snprintf(json, sizeof(json),
        "{\"worker\":\"%s\",\"request_no\":%" PRIu64
        ",\"path\":\"prototype\""
        ",\"operation\":\"%s\""
        ",\"a\":%.2f,\"b\":%.2f,\"result\":%.2f}\n",
        s_function_name,
        s->req_no,
        operation,
        s->operand_a,
        s->operand_b,
        result);
    if (jlen <= 0 || (size_t)jlen >= sizeof(json)) return -1;

    char pad_header[512];
    pad_header[0] = '\0';
    if (s_response_pad_bytes >= 16) {
        size_t pad_value_len = s_response_pad_bytes - 16;
        if (pad_value_len > 400) pad_value_len = 400;

        char pad_value[401];
        memset(pad_value, 'x', pad_value_len);
        pad_value[pad_value_len] = '\0';

        int phlen = snprintf(pad_header, sizeof(pad_header),
            "X-Bench2-Pad: %s\\r\\n", pad_value);
        if (phlen <= 0 || (size_t)phlen >= sizeof(pad_header)) return -1;
    }

    int rlen = snprintf(s->resp, sizeof(s->resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "%s"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        jlen,
        pad_header,
        s->should_close ? "close" : "keep-alive",
        json);
    if (rlen <= 0 || (size_t)rlen >= sizeof(s->resp)) return -1;

    s->resp_len = (size_t)rlen;
    s->resp_off = 0;
    s->state = WS_WRITE_RESPONSE;
    return epoll_mod_fd(s->fd, epoll_write_events());
}

static int session_parse_headers(worker_session_t *s)
{
    ssize_t hdr_end = find_subseq(s->buf, s->len, "\r\n\r\n");
    size_t sep_len = 4;
    if (hdr_end < 0) {
        hdr_end = find_subseq(s->buf, s->len, "\n\n");
        sep_len = 2;
    }
    if (hdr_end < 0) return 0;

    s->hdr_sz = (size_t)hdr_end + sep_len;
    long long cl = parse_content_length((const char *)s->buf, s->hdr_sz);
    if (cl < 0) cl = 0;
    if ((uint64_t)cl > WORKER_MAX_REQ_BYTES) return -1;

    s->body_target = (size_t)cl;
    s->body_in = (s->len > s->hdr_sz) ? s->len - s->hdr_sz : 0;
    if (s->body_in > s->body_target) s->body_in = s->body_target;
    s->should_close = has_connection_close((const char *)s->buf, s->hdr_sz);
    s->single_owner_hint = has_single_owner_hint((const char *)s->buf, s->hdr_sz);
    parse_request_operands(s->buf, s->hdr_sz, &s->operand_a, &s->operand_b);
    s->state = WS_READ_BODY;
    return 1;
}

static int session_relay(worker_session_t *s, const char *owner)
{
    bench2_keepalive_payload_t relay_payload;
    memset(&relay_payload, 0, sizeof(relay_payload));
    relay_payload.magic = BENCH2_KA_MAGIC;
    relay_payload.version = BENCH2_KA_VERSION;
    memcpy(&relay_payload.serial, &s->payload.serial, sizeof(relay_payload.serial));
    snprintf(relay_payload.target_function, sizeof(relay_payload.target_function), "%s", owner);

    int fd_to_relay = s->fd;
    if (fd_to_relay >= 0 && fd_to_relay < WORKER_MAX_FDS) {
        s_sessions[fd_to_relay] = NULL;
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd_to_relay, NULL);
    }
    s->fd = -1;
    if (s->ssl) wolfSSL_set_fd(s->ssl, -1);

    BENCH2_WORKER_TRACE("[bench2-worker] relay fd=%d req=%" PRIu64 " from=%s to=%s seq=%" PRIu64 "\n",
                        fd_to_relay,
                        s->req_no,
                        s_function_name,
                        owner,
                        relay_payload.serial.read_seq_num);

    session_close(s);
    return relay_to_gateway(fd_to_relay, &relay_payload, s_relay_socket);
}

static int session_peek_owner(worker_session_t *s)
{
    int ready = tls_record_ready(s->fd);
    if (ready <= 0) return ready;

    char owner[BENCH2_KA_TARGET_LEN];
    if (peek_owner_from_kernel(s->fd, &s->payload.serial, owner, sizeof(owner)) != 0)
        return -1;

    if (strcmp(owner, s_function_name) != 0) {
        (void)session_relay(s, owner);
        return 2;
    }

    session_reset_request(s, WS_READ_HEADERS);
    return 1;
}

static int session_flush_response(worker_session_t *s)
{
    while (s->resp_off < s->resp_len) {
        int n = wolfSSL_write(s->ssl,
                              s->resp + s->resp_off,
                              (int)(s->resp_len - s->resp_off));
        if (n > 0) {
            s->resp_off += (size_t)n;
            continue;
        }

        int err = wolfSSL_get_error(s->ssl, n);
        return wolfssl_want(s, err);
    }

    if (s->should_close) return -1;
    if (export_live_serial(s->ssl, &s->payload.serial) != 0) return -1;

    s->single_owner_direct_read = s->single_owner_hint != 0;
    s->req_no++;
    session_reset_request(s, s->single_owner_direct_read ? WS_READ_HEADERS : WS_PEEK_OWNER);
    if (epoll_mod_fd(s->fd, epoll_read_events()) != 0) return -1;
    return (s->single_owner_direct_read && wolfSSL_pending(s->ssl) > 0) ? 1 : 0;
}

static int session_advance(worker_session_t *s)
{
    for (;;) {
        switch (s->state) {
        case WS_PEEK_OWNER: {
            int rc = session_peek_owner(s);
            if (rc == 2) return 0;
            if (rc <= 0) return rc;
            continue;
        }

        case WS_READ_HEADERS: {
            int rc = session_tls_read_buffer(s);
            if (rc <= 0) return rc;
            rc = session_parse_headers(s);
            if (rc < 0) return -1;
            if (rc == 0) continue;
            if (s->body_in >= s->body_target)
                return session_build_response(s);
            continue;
        }

        case WS_READ_BODY: {
            int rc = session_tls_drain_body(s);
            if (rc <= 0) return rc;
            return session_build_response(s);
        }

        case WS_WRITE_RESPONSE: {
            int rc = session_flush_response(s);
            if (rc <= 0) return rc;
            continue;
        }
        }
    }
}

static void handoff_close(int fd)
{
    if (fd >= 0 && fd < WORKER_MAX_FDS) {
        handoff_conn_t *h = s_handoffs[fd];
        s_handoffs[fd] = NULL;
        if (h) {
            if (h->client_fd >= 0) close(h->client_fd);
            free(h);
        }
    }
    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

static void register_client_fd(int client_fd, const bench2_keepalive_payload_t *payload)
{
    if (payload->magic != BENCH2_KA_MAGIC || payload->version != BENCH2_KA_VERSION) {
        fprintf(stderr, "[bench2-worker] bad magic/version 0x%08x/%u\n",
                payload->magic, payload->version);
        close(client_fd);
        return;
    }

    worker_session_t *s = session_new(client_fd, payload);
    if (!s) return;

    if (epoll_add_fd(client_fd, epoll_read_events()) != 0) {
        fprintf(stderr, "[bench2-worker] epoll add client fd=%d failed: %s\n",
                client_fd, strerror(errno));
        session_close(s);
    }
}

static void handle_handoff_fd(int conn_fd)
{
    handoff_conn_t *h = (conn_fd >= 0 && conn_fd < WORKER_MAX_FDS)
        ? s_handoffs[conn_fd] : NULL;
    if (!h) {
        handoff_close(conn_fd);
        return;
    }

    int rc = handoff_recv_nb(h);
    if (rc == 0) return;

    int client_fd = h->client_fd;
    bench2_keepalive_payload_t payload = h->payload;
    if (rc == 1) h->client_fd = -1;
    handoff_close(conn_fd);

    if (rc < 0) return;
    register_client_fd(client_fd, &payload);
}

static void handle_listen_fd(int listen_fd)
{
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fprintf(stderr, "[bench2-worker] accept4 handoff failed: %s\n", strerror(errno));
            break;
        }
        if (conn_fd >= WORKER_MAX_FDS) {
            close(conn_fd);
            continue;
        }
        handoff_conn_t *h = calloc(1, sizeof(*h));
        if (!h) {
            close(conn_fd);
            continue;
        }
        h->fd = conn_fd;
        h->client_fd = -1;
        s_handoffs[conn_fd] = h;
        if (epoll_add_fd(conn_fd, epoll_read_events()) != 0) {
            handoff_close(conn_fd);
        }
    }
}

/* ─── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    const char *fn_name      = getenv("BENCH2_FUNCTION_NAME");
    const char *socket_dir   = getenv("BENCH2_SOCKET_DIR");
    const char *relay_socket = getenv("BENCH2_RELAY_SOCKET");
    const char *cert_file    = getenv("BENCH2_CERT");
    const char *key_file     = getenv("BENCH2_KEY");
    const char *pad_bytes_s  = getenv("BENCH2_RESPONSE_PAD_BYTES");

    if (!fn_name      || !fn_name[0])      fn_name      = "bench2-fn-a";
    if (!socket_dir   || !socket_dir[0])   socket_dir   = "/run/bench2";
    if (!relay_socket || !relay_socket[0]) relay_socket = "/run/bench2/relay.sock";
    if (!cert_file    || !cert_file[0])    cert_file    = "/certs/server.crt";
    if (!key_file     || !key_file[0])     key_file     = "/certs/server.key";

    if (pad_bytes_s && pad_bytes_s[0]) {
        char *ep = NULL;
        long parsed = strtol(pad_bytes_s, &ep, 10);
        if (ep && *ep == '\0' && parsed >= 0) {
            s_response_pad_bytes = (size_t)parsed;
        }
    }

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, fn_name);

    signal(SIGPIPE, SIG_IGN);

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "[bench2-worker] warning: mkdir %s: %s\n",
                socket_dir, strerror(errno));
    else
        chmod(socket_dir, 0777);

    wolfSSL_Init();
    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) { fprintf(stderr, "[bench2-worker] wolfSSL_CTX_new failed\n"); return 1; }
    s_wctx = wctx;
    s_function_name = fn_name;
    s_relay_socket = relay_socket;

    (void)wolfSSL_CTX_use_certificate_file(wctx, cert_file, SSL_FILETYPE_PEM);
    (void)wolfSSL_CTX_use_PrivateKey_file(wctx, key_file,   SSL_FILETYPE_PEM);

    int listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[bench2-worker] unix_server_socket failed: %s\n", strerror(errno));
        wolfSSL_CTX_free(wctx);
        return 1;
    }
    chmod(sock_path, 0777);
    if (set_nonblocking(listen_fd) != 0) {
        fprintf(stderr, "[bench2-worker] set_nonblocking(listen_fd) failed: %s\n", strerror(errno));
        close(listen_fd);
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    s_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epoll_fd < 0) {
        fprintf(stderr, "[bench2-worker] epoll_create1 failed: %s\n", strerror(errno));
        close(listen_fd);
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    if (epoll_add_fd(listen_fd, epoll_read_events()) != 0) {
        fprintf(stderr, "[bench2-worker] epoll_ctl add listen_fd failed: %s\n", strerror(errno));
        close(s_epoll_fd);
        close(listen_fd);
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    fprintf(stderr, "[bench2-worker] %s listening on %s, relay=%s [epoll multi-session]\n",
            fn_name, sock_path, relay_socket);

    struct epoll_event events[WORKER_MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(s_epoll_fd, events, WORKER_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[bench2-worker] epoll_wait failed: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t rev = events[i].events;

            if (fd == listen_fd) {
                handle_listen_fd(listen_fd);
                continue;
            }

            if (fd < 0 || fd >= WORKER_MAX_FDS) continue;

            if (s_handoffs[fd]) {
                if (rev & EPOLLIN) {
                    handle_handoff_fd(fd);
                } else if (rev & (EPOLLERR | EPOLLHUP
#ifdef EPOLLRDHUP
                           | EPOLLRDHUP
#endif
                           )) {
                    handoff_close(fd);
                }
                continue;
            }

            worker_session_t *s = s_sessions[fd];
            if (!s) {
                epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }

            if (rev & EPOLLERR) {
                session_close(s);
                continue;
            }

            if (rev & (EPOLLIN | EPOLLOUT)) {
                if (session_advance(s) < 0) {
                    session_close(s);
                }
                continue;
            }

            if (rev & (EPOLLHUP
#ifdef EPOLLRDHUP
                       | EPOLLRDHUP
#endif
                       )) {
                session_close(s);
                continue;
            }
        }
    }

    close(s_epoll_fd);
    close(listen_fd);
    wolfSSL_CTX_free(wctx);
    return 1;
}
