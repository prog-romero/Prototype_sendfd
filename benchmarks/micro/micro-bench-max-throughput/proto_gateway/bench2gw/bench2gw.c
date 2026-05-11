/*
 * bench2gw.c — C implementation of the bench2 prototype gateway shim.
 *
 * Key responsibility: for every incoming TLS connection:
 *   1. Accept + TLS handshake.
 *   2. Drive TLS handshake from the Go epoll loop.
 *   3. tls_read_peek(...) — decrypt without consuming kernel buffer.
 *   4. Populate bench2_keepalive_payload_t with the exported TLS serial.
 *   5. Return the raw TCP fd to Go so it can be forwarded to the right worker.
 */

#define _GNU_SOURCE

#include "bench2gw.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ─── internal struct ───────────────────────────────────────────────────── */

struct bench2gw_ctx {
    WOLFSSL_CTX *wctx;
};

typedef enum {
    BENCH2GW_CONN_HANDSHAKE = 0,
    BENCH2GW_CONN_PEEK,
    BENCH2GW_CONN_DONE,
} bench2gw_conn_state_t;

struct bench2gw_conn {
    bench2gw_ctx_t *ctx;
    int fd;
    WOLFSSL *ssl;
    tlspeek_ctx_t peek_ctx;
    bench2gw_conn_state_t state;
    int want_events; /* 1=POLLIN, 2=POLLOUT */
};

/* ─── wolfSSL cipher-suite helper ───────────────────────────────────────── */

static void set_cipher_from_name(tlspeek_ctx_t *peek, const char *name)
{
    if (!peek) return;
    if (!name) { peek->cipher_suite = TLSPEEK_AES_256_GCM; return; }
    if      (strstr(name, "CHACHA20"))  peek->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES-128"))  peek->cipher_suite = TLSPEEK_AES_128_GCM;
    else                               peek->cipher_suite = TLSPEEK_AES_256_GCM;
}

static void set_serial_cipher_from_name(tlspeek_serial_t *serial, const char *name)
{
    if (!serial) return;
    if (!name) { serial->cipher_suite = TLSPEEK_AES_256_GCM; return; }
    if      (strstr(name, "CHACHA20"))  serial->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES-128"))  serial->cipher_suite = TLSPEEK_AES_128_GCM;
    else                                 serial->cipher_suite = TLSPEEK_AES_256_GCM;
}

static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *out, size_t out_sz)
{
    if (!buf || !len || !out || !out_sz) return false;

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
        memcmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }

    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++) {
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            nlen = i;
            break;
        }
    }

    if (!nlen || nlen + 1 > out_sz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

static int export_live_serial(WOLFSSL *ssl, tlspeek_serial_t *serial)
{
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    const unsigned char *key = NULL;
    const unsigned char *iv = NULL;
    int key_sz = 0, iv_sz = 0;
    word64 peer_seq = 0;

    if (!ssl || !serial) return -1;

    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    set_serial_cipher_from_name(serial, wolfSSL_get_cipher_name(ssl));

    if (wolfSSL_tls_export(ssl, serial->tls_blob, &blob_sz) <= 0) return -1;
    serial->blob_sz = blob_sz;

    key = wolfSSL_GetClientWriteKey(ssl);
    iv = wolfSSL_GetClientWriteIV(ssl);
    key_sz = wolfSSL_GetKeySize(ssl);
    iv_sz = wolfSSL_GetIVSize(ssl);
    if (!key || !iv || key_sz <= 0 || iv_sz <= 0) return -1;

    size_t ks = (size_t)key_sz < sizeof(serial->client_write_key)
        ? (size_t)key_sz : sizeof(serial->client_write_key);
    size_t is = (size_t)iv_sz < sizeof(serial->client_write_iv)
        ? (size_t)iv_sz : sizeof(serial->client_write_iv);
    memcpy(serial->client_write_key, key, ks);
    memcpy(serial->client_write_iv, iv, is);

    if (wolfSSL_GetPeerSequenceNumber(ssl, &peer_seq) < 0) return -1;
    serial->read_seq_num = (uint64_t)peer_seq;
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

static int recvfd_with_state_nb(int unix_sock, int *received_fd,
                                void *payload, size_t payload_len)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct iovec iov = {
        .iov_base = payload,
        .iov_len = payload_len,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t rcvd;
    do {
        rcvd = recvmsg(unix_sock, &msg, MSG_DONTWAIT);
    } while (rcvd < 0 && errno == EINTR);

    if (rcvd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (rcvd == 0 || (size_t)rcvd < payload_len || (msg.msg_flags & MSG_CTRUNC))
        return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;

    memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
    return 1;
}

static int progressive_peek_owner(int client_fd,
                                  const tlspeek_serial_t *serial,
                                  unsigned char *peek_buf,
                                  size_t peek_buf_sz,
                                  int *peek_len_out,
                                  char *owner_out,
                                  size_t owner_out_sz)
{
    if (!serial || !peek_buf || peek_buf_sz == 0 || !owner_out || owner_out_sz == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t want = peek_buf_sz < 256 ? peek_buf_sz : 256;
    while (want > 0) {
        tlspeek_ctx_t peek_ctx;
        if (tlspeek_restore_peek_ctx(&peek_ctx, client_fd, serial) != 0) {
            return -1;
        }
        int peeked = tls_read_peek(&peek_ctx, peek_buf, want);
        tlspeek_free(&peek_ctx);
        if (peeked <= 0) {
            errno = EAGAIN;
            return -1;
        }

        if (peek_len_out) *peek_len_out = peeked;
        if (parse_request_owner(peek_buf, (size_t)peeked, owner_out, owner_out_sz)) {
            return 0;
        }

        if (want >= peek_buf_sz) break;
        want *= 2;
        if (want > peek_buf_sz) want = peek_buf_sz;
    }

    errno = EMSGSIZE;
    return -1;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

bench2gw_ctx_t *bench2gw_new_ctx(const char *cert_file, const char *key_file)
{
    if (!cert_file || !key_file) return NULL;

    wolfSSL_Init();

    bench2gw_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx->wctx) { free(ctx); return NULL; }

    if (wolfSSL_CTX_use_certificate_file(ctx->wctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(ctx->wctx, key_file,   SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx->wctx);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void bench2gw_free_ctx(bench2gw_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->wctx) wolfSSL_CTX_free(ctx->wctx);
    free(ctx);
}

bench2gw_conn_t *bench2gw_accept_start(bench2gw_ctx_t *ctx, int listen_fd)
{
    if (!ctx || !ctx->wctx || listen_fd < 0) {
        errno = EINVAL;
        return NULL;
    }

    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) return NULL;

    bench2gw_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        close(client_fd);
        errno = ENOMEM;
        return NULL;
    }

    conn->ctx = ctx;
    conn->fd = client_fd;
    conn->state = BENCH2GW_CONN_HANDSHAKE;
    conn->want_events = 1;
    conn->ssl = wolfSSL_new(ctx->wctx);
    if (!conn->ssl) {
        bench2gw_conn_free(conn);
        errno = ENOMEM;
        return NULL;
    }

    memset(&conn->peek_ctx, 0, sizeof(conn->peek_ctx));
    conn->peek_ctx.tcp_fd = client_fd;
    conn->peek_ctx.ssl = conn->ssl;

    wolfSSL_set_fd(conn->ssl, client_fd);
    wolfSSL_set_using_nonblock(conn->ssl, 1);
    wolfSSL_set_ex_data(conn->ssl, 0, &conn->peek_ctx);
    wolfSSL_set_tls13_secret_cb(conn->ssl, tlspeek_keylog_cb, &conn->peek_ctx);
    (void)set_nonblocking(client_fd);

    return conn;
}

int bench2gw_conn_fd(bench2gw_conn_t *conn)
{
    return conn ? conn->fd : -1;
}

int bench2gw_conn_events(bench2gw_conn_t *conn)
{
    return conn ? conn->want_events : 1;
}

int bench2gw_conn_step(bench2gw_conn_t *conn,
                       unsigned char *peek_buf,
                       size_t peek_buf_sz,
                       int *peek_len_out,
                       bench2_keepalive_payload_t *payload_out)
{
    if (!conn || conn->fd < 0 || !conn->ssl ||
        !peek_buf || peek_buf_sz == 0 || !payload_out) {
        errno = EINVAL;
        return -1;
    }

    if (conn->state == BENCH2GW_CONN_HANDSHAKE) {
        int rc = wolfSSL_accept(conn->ssl);
        if (rc != SSL_SUCCESS) {
            int err = wolfSSL_get_error(conn->ssl, rc);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                conn->want_events = (err == WOLFSSL_ERROR_WANT_WRITE) ? 2 : 1;
                return 0;
            }
            fprintf(stderr, "[bench2gw] wolfSSL_accept failed fd=%d err=%d\n",
                    conn->fd, err);
            fflush(stderr);
            return -1;
        }
        if (!conn->peek_ctx.keys_ready) {
            fprintf(stderr, "[bench2gw] TLS keys not ready after handshake fd=%d\n",
                    conn->fd);
            fflush(stderr);
            errno = EPROTO;
            return -1;
        }
        conn->state = BENCH2GW_CONN_PEEK;
        conn->want_events = 1;
    }

    if (conn->state == BENCH2GW_CONN_PEEK) {
        int ready = tls_record_ready(conn->fd);
        if (ready <= 0) {
            conn->want_events = 1;
            if (ready < 0) {
                fprintf(stderr, "[bench2gw] tls_record_ready failed fd=%d errno=%d\n",
                        conn->fd, errno);
                fflush(stderr);
            }
            return ready;
        }

        memset(payload_out, 0, sizeof(*payload_out));
        payload_out->magic = BENCH2_KA_MAGIC;
        payload_out->version = BENCH2_KA_VERSION;

        if (export_live_serial(conn->ssl, &payload_out->serial) != 0) {
            fprintf(stderr, "[bench2gw] export_live_serial failed fd=%d\n",
                    conn->fd);
            fflush(stderr);
            errno = EIO;
            return -1;
        }
        if (progressive_peek_owner(conn->fd,
                                   &payload_out->serial,
                                   peek_buf,
                                   peek_buf_sz,
                                   peek_len_out,
                                   payload_out->target_function,
                                   sizeof(payload_out->target_function)) != 0) {
            fprintf(stderr, "[bench2gw] progressive_peek_owner failed fd=%d errno=%d\n",
                    conn->fd, errno);
            fflush(stderr);
            return -1;
        }

        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_set_fd(conn->ssl, -1);
        wolfSSL_free(conn->ssl);
        conn->ssl = NULL;
        tlspeek_free(&conn->peek_ctx);
        conn->state = BENCH2GW_CONN_DONE;
        return 1;
    }

    errno = EINVAL;
    return -1;
}

int bench2gw_conn_take_fd(bench2gw_conn_t *conn)
{
    if (!conn) return -1;
    int fd = conn->fd;
    conn->fd = -1;
    return fd;
}

void bench2gw_conn_free(bench2gw_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
    }
    if (conn->fd >= 0) close(conn->fd);
    free(conn);
}

/*
 * bench2gw_accept_peek_export
 *
 * Compatibility one-shot wrapper. The main Go server uses the incremental
 * bench2gw_conn_* API so handshakes and peeks stay epoll-driven.
 *
 * Returns the raw TCP fd only when the entire handshake + peek completes
 * immediately; otherwise returns -1/EAGAIN and closes the accepted fd.
 */
int bench2gw_accept_peek_export(
    bench2gw_ctx_t               *ctx,
    int                           listen_fd,
    unsigned char                *peek_buf,
    size_t                        peek_buf_sz,
    int                          *peek_len_out,
    bench2_keepalive_payload_t   *payload_out)
{
    if (!ctx || !ctx->wctx || listen_fd < 0 ||
        !peek_buf || peek_buf_sz == 0 || !payload_out) {
        errno = EINVAL;
        return -1;
    }

    bench2gw_conn_t *conn = bench2gw_accept_start(ctx, listen_fd);
    if (!conn) return -1;

    int rc = bench2gw_conn_step(conn, peek_buf, peek_buf_sz, peek_len_out, payload_out);
    if (rc != 1) {
        if (rc == 0) errno = EAGAIN;
        bench2gw_conn_free(conn);
        return -1;
    }

    int client_fd = bench2gw_conn_take_fd(conn);
    bench2gw_conn_free(conn);
    return client_fd;
}

int bench2gw_send_fd(
    const char                         *unix_sock_path,
    int                                 client_fd,
    const bench2_keepalive_payload_t   *payload,
    size_t                              payload_len)
{
    if (!unix_sock_path || !payload || payload_len == 0 || client_fd < 0) {
        if (client_fd >= 0) close(client_fd);
        errno = EINVAL;
        return -1;
    }

    int uds_fd = unix_client_connect(unix_sock_path);
    if (uds_fd < 0) { close(client_fd); return -1; }

    int rc = sendfd_with_state(uds_fd, client_fd, payload, payload_len);
    close(uds_fd);
    if (rc != 0) { close(client_fd); return -1; }
    return 0;
}

int bench2gw_unix_server(const char *sock_path)
{
    int fd = unix_server_socket(sock_path, 4096);
    if (fd < 0) return -1;
    (void)chmod(sock_path, 0777);
    return fd;
}

int bench2gw_accept_recv(int listen_fd, bench2_keepalive_payload_t *payload_out)
{
    if (listen_fd < 0 || !payload_out) { errno = EINVAL; return -1; }

    int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (conn_fd < 0) return -1;

    int client_fd = -1;
    int rc = recvfd_with_state_nb(conn_fd, &client_fd, payload_out, sizeof(*payload_out));
    if (rc != 1) {
        close(conn_fd);
        if (client_fd >= 0) close(client_fd);
        if (rc == 0) errno = EAGAIN;
        return -1;
    }
    close(conn_fd);
    return client_fd;
}
