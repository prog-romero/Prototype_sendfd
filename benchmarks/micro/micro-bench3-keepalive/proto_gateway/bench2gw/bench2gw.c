/*
 * bench2gw.c — C implementation of the bench2 prototype gateway shim.
 *
 * Key responsibility: for every incoming TLS connection:
 *   1. Accept + TLS handshake.
 *   2. bench2_wait_readable(tcp_fd) — wait until encrypted data is in kernel buffer.
 *   3. top1 = bench2_rdtsc()        — stamp: data is there, we are about to peek.
 *   4. tls_read_peek(...)            — decrypt without consuming kernel buffer.
 *   5. Populate bench2_keepalive_payload_t with serial, top1, cntfrq.
 *   6. Return the raw TCP fd to Go so it can be forwarded to the right worker.
 */

#define _GNU_SOURCE

#include "bench2gw.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

/* ─── internal struct ───────────────────────────────────────────────────── */

struct bench2gw_ctx {
    WOLFSSL_CTX *wctx;
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

    size_t want = peek_buf_sz < 1024 ? peek_buf_sz : 1024;
    while (want > 0) {
        tlspeek_ctx_t peek_ctx;
        if (tlspeek_restore_peek_ctx(&peek_ctx, client_fd, serial) != 0) {
            return -1;
        }

        int peeked = tls_read_peek(&peek_ctx, peek_buf, want);
        tlspeek_free(&peek_ctx);
        if (peeked <= 0) {
            errno = EIO;
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

/*
 * bench2gw_accept_peek_export
 *
 * Accepting one TCP connection, performing the TLS handshake, then:
 *   bench2_wait_readable(tcp_fd)   — spin until TLS bytes are in kernel buffer
 *   top1 = bench2_rdtsc()          — stamp before any read
 *   tls_read_peek(...)             — stateless decrypt, no buffer consumed
 *   build payload (serial, top1, cntfrq, top1_set=1)
 *
 * Returns the raw TCP fd, or -1 on error.
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

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return -1;

    WOLFSSL *ssl = wolfSSL_new(ctx->wctx);
    if (!ssl) { close(client_fd); errno = ENOMEM; return -1; }

    wolfSSL_set_fd(ssl, client_fd);

    tlspeek_ctx_t peek_ctx;
    memset(&peek_ctx, 0, sizeof(peek_ctx));
    peek_ctx.tcp_fd = client_fd;
    peek_ctx.ssl    = ssl;
    wolfSSL_set_ex_data(ssl, 0, &peek_ctx);
    wolfSSL_set_tls13_secret_cb(ssl, tlspeek_keylog_cb, &peek_ctx);

    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        wolfSSL_free(ssl);
        close(client_fd);
        return -1;
    }

    if (!peek_ctx.keys_ready) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
        close(client_fd);
        errno = EPROTO;
        return -1;
    }

    /* ── top1: wait for data in kernel buffer, then stamp ── */
    bench2_wait_readable(client_fd);
    uint64_t top1   = bench2_rdtsc();
    uint64_t cntfrq = bench2_cntfrq();

    /* ── Build payload ── */
    memset(payload_out, 0, sizeof(*payload_out));
    payload_out->magic       = BENCH2_KA_MAGIC;
    payload_out->version     = BENCH2_KA_VERSION;
    payload_out->top1_rdtsc  = top1;
    payload_out->cntfrq      = cntfrq;
    payload_out->top1_set    = 1;

    if (export_live_serial(ssl, &payload_out->serial) != 0) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
        close(client_fd);
        errno = EIO;
        return -1;
    }

    if (progressive_peek_owner(client_fd,
                               &payload_out->serial,
                               peek_buf,
                               peek_buf_sz,
                               peek_len_out,
                               payload_out->target_function,
                               sizeof(payload_out->target_function)) != 0) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
        close(client_fd);
        return -1;
    }

    /* Release wolfSSL without closing the fd */
    wolfSSL_set_quiet_shutdown(ssl, 1);
    wolfSSL_set_fd(ssl, -1);
    wolfSSL_free(ssl);
    tlspeek_free(&peek_ctx);

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

    int conn_fd  = unix_accept(listen_fd);
    if (conn_fd < 0) return -1;

    int client_fd = -1;
    if (recvfd_with_state(conn_fd, &client_fd, payload_out, sizeof(*payload_out)) != 0) {
        close(conn_fd);
        if (client_fd >= 0) close(client_fd);
        return -1;
    }
    close(conn_fd);
    return client_fd;
}
