/*
 * tlspeek.c — Core library: keylog callback + tls_read_peek().
 *
 * This is the heart of the novel contribution:
 *
 *  1.  tlspeek_keylog_cb()  fires after each wolfSSL handshake.
 *      It derives write keys and IVs from traffic secrets using
 *      HKDF-Expand-Label (RFC 8446 §7.1) and stores them in the
 *      per-connection tlspeek_ctx_t.
 *
 *  2.  tls_read_peek()  performs stateless AEAD decryption of the
 *      first TLS 1.3 application data record WITHOUT consuming the
 *      kernel TCP receive buffer (MSG_PEEK).
 */

#include "tlspeek.h"
#include "tlspeek_crypto.h"
#include "tlspeek_serial.h"
#include "../common/tlspeek_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/socket.h>  /* recv / MSG_PEEK */

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/hash.h>   /* WC_SHA256, WC_SHA384 */
#include <wolfssl/wolfcrypt/hmac.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Keylog callback helpers                                                      */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * Derive write_key and write_iv from a hex-encoded traffic secret.
 *
 * key_out      : TLSPEEK_KEY_SIZE bytes output buffer
 * iv_out       : TLSPEEK_IV_SIZE bytes output buffer
 * hex_secret   : null-terminated hex string from the keylog line
 * key_len      : 32 (AES-256) or 16 (AES-128)
 * hash_algo    : WC_SHA384 (AES-256-GCM) or WC_SHA256 (AES-128 / ChaCha20)
 */
static int derive_keys_from_raw_secret(
        uint8_t    *key_out,
        uint8_t    *iv_out,
        const uint8_t *secret,
        size_t      secret_len,
        size_t      key_len,
        int         hash_algo)
{
    /* key = HKDF-Expand-Label(secret, "key", "", key_len) */
    if (tls13_hkdf_expand_label(secret, secret_len,
                                 "key",
                                 NULL, 0,
                                 key_out, key_len,
                                 hash_algo) != 0)
    {
        fprintf(stderr, "[keylog] HKDF for 'key' failed\n");
        return -1;
    }

    /* iv = HKDF-Expand-Label(secret, "iv", "", 12) */
    if (tls13_hkdf_expand_label(secret, secret_len,
                                 "iv",
                                 NULL, 0,
                                 iv_out, TLSPEEK_IV_SIZE,
                                 hash_algo) != 0)
    {
        fprintf(stderr, "[keylog] HKDF for 'iv' failed\n");
        return -1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * tlspeek_keylog_cb — wolfSSL keylog callback (NSS Key Log Format).
 *
 * Installed with:  wolfSSL_CTX_set_keylog_callback(ctx, tlspeek_keylog_cb);
 *
 * Called for EVERY keylog line.  We care about:
 *   CLIENT_TRAFFIC_SECRET_0  <client_random>  <secret>
 *   SERVER_TRAFFIC_SECRET_0  <client_random>  <secret>
 *
 * The per-connection ctx is retrieved via wolfSSL_get_ex_data(ssl, 0).
 */
int tlspeek_keylog_cb(WOLFSSL *ssl, int id, const unsigned char *secret,
                      int secretSz, void *ctx_arg)
{
    if (!secret) return 0;
    
    /* Retrieve per-connection context from the argument */
    tlspeek_ctx_t *ctx = (tlspeek_ctx_t *)ctx_arg;
    
    /* Fallback to ex_data if arg is NULL (for backward compatibility or other callers) */
    if (!ctx) {
        ctx = (tlspeek_ctx_t *)wolfSSL_get_ex_data(ssl, 0);
    }
    
    if (!ctx) {
        /* If we still have no context, we can't store the keys, but we should at least log that we were called */
        TLSPEEK_VLOG("[keylog] Callback called (id=%d, sz=%d) but NO CONTEXT FOUND\n", id, secretSz);
        return 0;
    }

        TLSPEEK_VLOG("[keylog] Callback called (id=%d, sz=%d)\n", id, secretSz);

    /* 
     * Determine cipher suite from the secret size:
     *   32 bytes → SHA-256 → AES-128-GCM / ChaCha20
     *   48 bytes → SHA-384 → AES-256-GCM
     */
    int    hash_algo;
    size_t key_len;

    if (secretSz == 48) {
        hash_algo = WC_SHA384;
        key_len   = 32;
        ctx->cipher_suite = TLSPEEK_AES_256_GCM;
    } else if (secretSz == 32) {
        hash_algo = WC_SHA256;
        key_len   = 16;
        ctx->cipher_suite = TLSPEEK_AES_128_GCM;
    } else {
        return 0;
    }

    if (id == CLIENT_TRAFFIC_SECRET) {
        TLSPEEK_VLOG("[keylog] Intercepted CLIENT_TRAFFIC_SECRET (%d bytes)\n", secretSz);
        derive_keys_from_raw_secret(ctx->client_write_key,
                                     ctx->client_write_iv,
                                     secret, secretSz,
                                     key_len, hash_algo);
    } else if (id == SERVER_TRAFFIC_SECRET) {
        TLSPEEK_VLOG("[keylog] Intercepted SERVER_TRAFFIC_SECRET (%d bytes)\n", secretSz);
        derive_keys_from_raw_secret(ctx->server_write_key,
                                     ctx->server_write_iv,
                                     secret, secretSz,
                                     key_len, hash_algo);
        ctx->keys_ready = 1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

int tlspeek_init(tlspeek_ctx_t *ctx, WOLFSSL *ssl, int tcp_fd)
{
    if (!ctx || !ssl) return -1;

    ctx->ssl    = ssl;
    ctx->tcp_fd = tcp_fd;

    if (!ctx->keys_ready) {
        fprintf(stderr,
                "[tlspeek] ERROR: keys_ready=0 after handshake — "
                "keylog callback did not fire. "
                "Did you install it with wolfSSL_CTX_set_keylog_callback?\n");
        return -1;
    }

    ctx->read_seq_num  = 0;
    ctx->write_seq_num = 0;

    TLSPEEK_VLOG("[tlspeek] Context initialized: tcp_fd=%d cipher=0x%04X\n",
                 tcp_fd, (unsigned)ctx->cipher_suite);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int tls_read_peek(tlspeek_ctx_t *ctx, uint8_t *buf, size_t size)
{
    if (!ctx || !buf || size == 0) return -1;
    if (!ctx->keys_ready) {
        fprintf(stderr, "[tlspeek] ERROR: keys not ready\n");
        return -1;
    }

    /* ── Sub-step 2a: MSG_PEEK — read encrypted bytes WITHOUT consuming ── */
    uint8_t raw[TLSPEEK_HEADER_SIZE + TLSPEEK_MAX_RECORD + TLSPEEK_TAG_SIZE];

    TLSPEEK_VLOG("[tlspeek] MSG_PEEK recv on fd=%d (up to %zu bytes)...\n",
                 ctx->tcp_fd, sizeof(raw));

    ssize_t raw_len = recv(ctx->tcp_fd, raw, sizeof(raw), MSG_PEEK);
    if (raw_len <= 0) {
        if (raw_len == 0)
            fprintf(stderr, "[tlspeek] recv(MSG_PEEK): connection closed\n");
        else
            perror("[tlspeek] recv(MSG_PEEK) failed");
        return -1;
    }

    TLSPEEK_VLOG("[tlspeek] MSG_PEEK got %zd bytes — kernel buffer UNCHANGED\n",
                 raw_len);

    /* ── Sub-step 2b: Parse TLS 1.3 record header (5 bytes) ── */
    if (raw_len < TLSPEEK_HEADER_SIZE) {
        fprintf(stderr,
                "[tlspeek] too few bytes for TLS header: %zd\n", raw_len);
        return -1;
    }

    uint8_t  record_type = raw[0];
    /* uint16 version     = raw[1..2] — should be 0x0303 */
    uint16_t record_len  = (uint16_t)((raw[3] << 8) | raw[4]);

    TLSPEEK_VLOG("[tlspeek] TLS record: type=0x%02X version=0x%02X%02X len=%u\n",
                 record_type, raw[1], raw[2], record_len);

    if (record_type != 0x17) {
        fprintf(stderr,
                "[tlspeek] ERROR: expected Application Data (0x17), "
                "got 0x%02X — first record may still be a handshake?\n",
                record_type);
        return -1;
    }

    if ((size_t)raw_len < (size_t)(TLSPEEK_HEADER_SIZE + record_len)) {
        fprintf(stderr,
                "[tlspeek] incomplete record in peek buffer "
                "(got %zd, need %d)\n",
                raw_len, TLSPEEK_HEADER_SIZE + record_len);
        return -1;
    }

    if (record_len <= TLSPEEK_TAG_SIZE) {
        fprintf(stderr,
                "[tlspeek] record_len %u too small (min %d for auth tag)\n",
                record_len, TLSPEEK_TAG_SIZE);
        return -1;
    }

    /* ── Sub-step 2c: Compute nonce (RFC 8446 §5.3) ── */
    uint8_t nonce[TLSPEEK_IV_SIZE];
    tls13_compute_nonce(ctx->client_write_iv, ctx->read_seq_num, nonce);
    /*
     * CRITICAL: read_seq_num is NOT incremented here.
     * This is the "stateless" property.
     */

    /* Pointers into the raw buffer */
    const uint8_t *ciphertext = raw + TLSPEEK_HEADER_SIZE;
    size_t         ct_len     = record_len - TLSPEEK_TAG_SIZE;  /* without tag */
    const uint8_t *auth_tag   = ciphertext + ct_len;
    const uint8_t *aad        = raw;  /* TLS record header = AAD */

    TLSPEEK_VLOG("[tlspeek] Decrypting: ct_len=%zu tag_offset=%zu\n",
                 ct_len, ct_len);

    /* ── Sub-step 2d: Stateless AEAD decryption ── */
    uint8_t plaintext[TLSPEEK_MAX_RECORD];
    int     ret;

    switch (ctx->cipher_suite) {
    case TLSPEEK_AES_256_GCM:
    case TLSPEEK_AES_128_GCM:
        ret = aead_aes_gcm_decrypt(
            ctx->client_write_key,
            nonce,
            aad,        TLSPEEK_HEADER_SIZE,
            ciphertext, ct_len,
            auth_tag,
            plaintext
        );
        break;

    case TLSPEEK_CHACHA20_POLY:
        ret = aead_chacha20_poly1305_decrypt(
            ctx->client_write_key,
            nonce,
            aad,        TLSPEEK_HEADER_SIZE,
            ciphertext, ct_len,
            auth_tag,
            plaintext
        );
        break;

    default:
        fprintf(stderr, "[tlspeek] unsupported cipher suite 0x%04X\n",
                (unsigned)ctx->cipher_suite);
        return -1;
    }

    if (ret != 0) {
        fprintf(stderr, "[tlspeek] stateless decryption failed\n");
        return -1;
    }

    /* ── Sub-step 2e: Strip TLS 1.3 inner content type (last byte) ── */
    /*
     * TLS 1.3 appends a 1-byte inner content type at the end of the
     * plaintext (before the auth tag).  The real content is everything
     * except the last byte.
     * RFC 8446 §5.4: TLSInnerPlaintext = { content_data || ContentType type }
     */
    if (ct_len == 0) {
        fprintf(stderr, "[tlspeek] empty plaintext after decryption\n");
        return -1;
    }

    size_t plaintext_len = ct_len - 1;  /* strip inner content type byte */

    uint8_t inner_type = plaintext[plaintext_len];
    TLSPEEK_VLOG("[tlspeek] Inner content type = 0x%02X, plaintext_len=%zu\n",
                 inner_type, plaintext_len);

    if (inner_type != 0x17) {
        fprintf(stderr,
                "[tlspeek] WARNING: inner content type 0x%02X != 0x17 "
                "(Application Data) — may be alert or handshake\n",
                inner_type);
    }

    /* ── Copy decrypted plaintext to caller's buffer ── */
    size_t copy_len = plaintext_len < size ? plaintext_len : size;
    memcpy(buf, plaintext, copy_len);

    TLSPEEK_VLOG("[tlspeek] SUCCESS: %zu plaintext bytes returned. Kernel buffer UNCHANGED, seq_num UNCHANGED (%llu)\n",
                 copy_len, (unsigned long long)ctx->read_seq_num);

    return (int)copy_len;
}

/* ─────────────────────────────────────────────────────────────────────────── */

void tlspeek_free(tlspeek_ctx_t *ctx)
{
    if (!ctx) return;
    /* Zero out key material for security */
    memset(ctx->client_write_key, 0, sizeof(ctx->client_write_key));
    memset(ctx->server_write_key, 0, sizeof(ctx->server_write_key));
    memset(ctx->client_write_iv,  0, sizeof(ctx->client_write_iv));
    memset(ctx->server_write_iv,  0, sizeof(ctx->server_write_iv));
    ctx->keys_ready = 0;
    /* NOTE: we do NOT close tcp_fd or free wolfSSL — caller manages those */
}
