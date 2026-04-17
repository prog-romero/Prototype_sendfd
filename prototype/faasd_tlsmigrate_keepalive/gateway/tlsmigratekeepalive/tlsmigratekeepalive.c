#include "tlsmigratekeepalive.h"

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

struct tlsmigratekeepalive_ctx {
    WOLFSSL_CTX* wctx;
};

static void set_cipher_suite_from_name(tlspeek_ctx_t* peek_ctx, const char* cipher_name) {
    if (!peek_ctx) {
        return;
    }
    if (!cipher_name) {
        peek_ctx->cipher_suite = TLSPEEK_AES_256_GCM;
        return;
    }
    if (strstr(cipher_name, "CHACHA20")) {
        peek_ctx->cipher_suite = TLSPEEK_CHACHA20_POLY;
    } else if (strstr(cipher_name, "AES-128")) {
        peek_ctx->cipher_suite = TLSPEEK_AES_128_GCM;
    } else {
        peek_ctx->cipher_suite = TLSPEEK_AES_256_GCM;
    }
}

tlsmigratekeepalive_ctx_t* tlsmigratekeepalive_new_ctx(const char* cert_file, const char* key_file) {
    tlsmigratekeepalive_ctx_t* ctx;

    if (!cert_file || !key_file) {
        return NULL;
    }

    wolfSSL_Init();

    ctx = (tlsmigratekeepalive_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx->wctx) {
        free(ctx);
        return NULL;
    }

    if (wolfSSL_CTX_use_certificate_file(ctx->wctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx->wctx);
        free(ctx);
        return NULL;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx->wctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx->wctx);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void tlsmigratekeepalive_free_ctx(tlsmigratekeepalive_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->wctx) {
        wolfSSL_CTX_free(ctx->wctx);
    }
    free(ctx);
}

int tlsmigratekeepalive_accept_peek_export(
    tlsmigratekeepalive_ctx_t* ctx,
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    tlsmigrate_keepalive_payload_t* payload_out) {

    int client_fd;
    WOLFSSL* ssl;
    tlspeek_ctx_t peek_ctx;
    int headers_len;
    int export_ret;
    tlspeek_serial_t* serial;

    if (!ctx || !ctx->wctx || listen_fd < 0 || !headers_buf || headers_buf_sz == 0 || !payload_out) {
        errno = EINVAL;
        return -1;
    }

    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        return -1;
    }

    ssl = wolfSSL_new(ctx->wctx);
    if (!ssl) {
        close(client_fd);
        errno = ENOMEM;
        return -1;
    }
    wolfSSL_set_fd(ssl, client_fd);

    memset(&peek_ctx, 0, sizeof(peek_ctx));
    peek_ctx.tcp_fd = client_fd;
    peek_ctx.ssl = ssl;
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

    set_cipher_suite_from_name(&peek_ctx, wolfSSL_get_cipher_name(ssl));

    headers_len = tls_read_peek(&peek_ctx, (uint8_t*)headers_buf, headers_buf_sz);
    if (headers_len <= 0) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
        close(client_fd);
        errno = EIO;
        return -1;
    }

    if (headers_len_out) {
        *headers_len_out = headers_len;
    }

    memset(payload_out, 0, sizeof(*payload_out));
    payload_out->magic = TLSMIGRATE_KEEPALIVE_MAGIC;
    payload_out->version = TLSMIGRATE_KEEPALIVE_VERSION;

    serial = &payload_out->serial;
    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    serial->cipher_suite = (uint32_t)peek_ctx.cipher_suite;
    memcpy(serial->client_write_key, peek_ctx.client_write_key, sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv, peek_ctx.client_write_iv, sizeof(serial->client_write_iv));
    serial->read_seq_num = peek_ctx.read_seq_num;
    serial->blob_sz = TLSPEEK_MAX_EXPORT_SZ;

    export_ret = wolfSSL_tls_export(ssl, serial->tls_blob, &serial->blob_sz);
    if (export_ret <= 0) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
        close(client_fd);
        errno = EIO;
        return -1;
    }

    wolfSSL_set_quiet_shutdown(ssl, 1);
    wolfSSL_set_fd(ssl, -1);
    wolfSSL_free(ssl);
    tlspeek_free(&peek_ctx);

    return client_fd;
}

int tlsmigratekeepalive_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const tlsmigrate_keepalive_payload_t* payload,
    size_t payload_len) {

    int uds_fd;
    int rc;

    if (!unix_sock_path || !payload || payload_len == 0 || client_fd < 0) {
        if (client_fd >= 0) {
            close(client_fd);
        }
        errno = EINVAL;
        return -1;
    }

    uds_fd = unix_client_connect(unix_sock_path);
    if (uds_fd < 0) {
        close(client_fd);
        return -1;
    }

    rc = sendfd_with_state(uds_fd, client_fd, payload, payload_len);
    close(uds_fd);
    if (rc != 0) {
        close(client_fd);
        return -1;
    }

    return 0;
}

int tlsmigratekeepalive_unix_server(const char* sock_path) {
    int fd;

    fd = unix_server_socket(sock_path, 4096);
    if (fd < 0) {
        return -1;
    }
    (void)chmod(sock_path, 0777);
    return fd;
}

int tlsmigratekeepalive_accept_recv(int listen_fd, tlsmigrate_keepalive_payload_t* payload_out) {
    int conn_fd;
    int client_fd = -1;

    if (listen_fd < 0 || !payload_out) {
        errno = EINVAL;
        return -1;
    }

    conn_fd = unix_accept(listen_fd);
    if (conn_fd < 0) {
        return -1;
    }

    if (recvfd_with_state(conn_fd, &client_fd, payload_out, sizeof(*payload_out)) != 0) {
        close(conn_fd);
        if (client_fd >= 0) {
            close(client_fd);
        }
        return -1;
    }

    close(conn_fd);
    return client_fd;
}