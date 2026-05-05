#ifndef TLSPEEK_H
#define TLSPEEK_H

#include <stdint.h>
#include <stddef.h>

/*
 * wolfSSL must be configured with:
 *   --enable-tls13 --enable-aesctr --enable-aesgcm --enable-chacha
 *   --enable-hkdf --enable-keylog-export
 */
/* Ensure wolfSSL features are enabled for this translation unit */
#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/kdf.h>

/* ─── Constants ──────────────────────────────────────────────────────────── */

#define TLSPEEK_MAGIC         0x544C5350U  /* "TLSP" */
#define TLSPEEK_KEY_SIZE      32           /* AES-256 key size           */
#define TLSPEEK_IV_SIZE       12           /* GCM / ChaCha20 nonce size  */
#define TLSPEEK_TAG_SIZE      16           /* GCM / Poly1305 auth tag    */
#define TLSPEEK_MAX_RECORD    16640        /* TLS 1.3 max app-data payload (2^14 + 256) */
#define TLSPEEK_HEADER_SIZE   5            /* TLS record header size      */

/* ─── Cipher suite identifiers (TLS 1.3 IANA values) ────────────────────── */

typedef enum {
    TLSPEEK_AES_128_GCM  = 0x1301,  /* TLS_AES_128_GCM_SHA256        */
    TLSPEEK_AES_256_GCM  = 0x1302,  /* TLS_AES_256_GCM_SHA384        */
    TLSPEEK_CHACHA20_POLY = 0x1303, /* TLS_CHACHA20_POLY1305_SHA256  */
} tlspeek_cipher_t;

/* ─── Live context (lives in the gateway per-connection) ─────────────────── */

typedef struct {
    uint8_t          client_write_key[TLSPEEK_KEY_SIZE];
    uint8_t          server_write_key[TLSPEEK_KEY_SIZE];
    uint8_t          client_write_iv[TLSPEEK_IV_SIZE];
    uint8_t          server_write_iv[TLSPEEK_IV_SIZE];
    uint64_t         read_seq_num;   /* next expected record seq number  */
    uint64_t         write_seq_num;
    tlspeek_cipher_t cipher_suite;
    int              tcp_fd;         /* raw TCP socket file descriptor   */
    WOLFSSL         *ssl;            /* wolfSSL session (gateway owns)   */
    int              keys_ready;     /* 1 after keylog callback fires    */
} tlspeek_ctx_t;

/* ─── Serialisable state (sent to worker via Unix socket) ────────────────── */

#define TLSPEEK_MAX_EXPORT_SZ 16384
#define TLSPEEK_MAX_REQUEST_SZ 8192

typedef struct {
    uint32_t magic;
    uint32_t cipher_suite;
    uint8_t  client_write_key[TLSPEEK_KEY_SIZE];
    uint8_t  client_write_iv[TLSPEEK_IV_SIZE];
    uint64_t read_seq_num;
    unsigned char tls_blob[TLSPEEK_MAX_EXPORT_SZ];
    unsigned int  blob_sz;
    char          http_request[TLSPEEK_MAX_REQUEST_SZ];
    int           request_len;
} tlspeek_serial_t;

/* ─── Public API ─────────────────────────────────────────────────────────── */

/**
 * tlspeek_init() — Initialize the context after a completed wolfSSL handshake.
 *
 * The keylog callback must have been installed on the CTX BEFORE the handshake.
 * The callback populates the key material into this ctx via wolfSSL_set_ex_data.
 *
 * @return 0 on success, -1 if keys_ready is not set.
 */
int tlspeek_init(tlspeek_ctx_t *ctx, WOLFSSL *ssl, int tcp_fd);

/**
 * tls_read_peek() — The core function.
 *
 * Reads decrypted TLS 1.3 application data into buf WITHOUT consuming
 * the kernel TCP receive buffer and WITHOUT advancing the sequence number.
 *
 * Internally:
 *   1. recv(tcp_fd, raw, TLSPEEK_MAX_RECORD + TLSPEEK_HEADER_SIZE, MSG_PEEK)
 *   2. Parse TLS 1.3 record header (expect Content-Type 0x17)
 *   3. Compute nonce = client_write_iv XOR read_seq_num (RFC 8446 §5.3)
 *   4. Stateless AES-GCM or ChaCha20-Poly1305 decryption
 *   5. Strip TLS 1.3 inner content type (last byte of plaintext)
 *   6. Copy result into buf
 *
 * INVARIANTS after return:
 *   - Kernel buffer:      UNCHANGED (MSG_PEEK)
 *   - ctx->read_seq_num:  UNCHANGED (stateless)
 *   - wolfSSL state:      UNCHANGED (no wolfSSL_read called)
 *
 * @return number of plaintext bytes, or -1 on error.
 */
int tls_read_peek(tlspeek_ctx_t *ctx, uint8_t *buf, size_t size);

/**
 * tlspeek_serialize() — Pack context into wire-transferable struct.
 */
void tlspeek_serialize(const tlspeek_ctx_t *ctx, tlspeek_serial_t *serial);

/**
 * tlspeek_restore() — Restore TLS session state from a serial struct.
 *
 * Called by the worker after receiving the fd.  Sets keys, IVs, and
 * sequence numbers on the provided wolfSSL* without performing a new
 * handshake.
 *
 * @return 0 on success, -1 on error.
 */
int tlspeek_restore(WOLFSSL *ssl, const tlspeek_serial_t *serial);

/**
 * tlspeek_restore_peek_ctx() — Rebuild the minimal stateless peek context.
 *
 * Used on the worker side when the caller wants to inspect encrypted data in
 * the kernel buffer with tls_read_peek() before handing control back to
 * wolfSSL_read().
 *
 * @return 0 on success, -1 on validation error.
 */
int tlspeek_restore_peek_ctx(tlspeek_ctx_t *ctx, int tcp_fd,
                             const tlspeek_serial_t *serial);

/**
 * tlspeek_free() — Release any resources held by the context.
 *
 * Does NOT close tcp_fd and does NOT free the wolfSSL session.
 */
void tlspeek_free(tlspeek_ctx_t *ctx);

#ifndef HAVE_SECRET_CALLBACK
enum Tls13Secret {
    CLIENT_EARLY_TRAFFIC_SECRET,
    CLIENT_HANDSHAKE_TRAFFIC_SECRET,
    SERVER_HANDSHAKE_TRAFFIC_SECRET,
    CLIENT_TRAFFIC_SECRET,
    SERVER_TRAFFIC_SECRET,
    EARLY_EXPORTER_SECRET,
    EXPORTER_SECRET
};
#endif

/**
 * tlspeek_keylog_cb() — wolfSSL TLS 1.3 secret callback (Tls13SecretCb).
 *
 * This matches the internal WOLFSSL->tls13KeyLogCb hook.
 * It receives raw traffic secrets directly from the wolfSSL engine.
 */
int tlspeek_keylog_cb(WOLFSSL *ssl, int id, const unsigned char *secret,
                      int secretSz, void *ctx);

#endif /* TLSPEEK_H */
