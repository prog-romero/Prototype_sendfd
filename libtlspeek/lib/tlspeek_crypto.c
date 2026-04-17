/*
 * tlspeek_crypto.c — Stateless AEAD decryption and cryptographic helpers.
 *
 * Uses wolfCrypt raw APIs (not wolfSSL TLS layer):
 *   wc_AesGcmDecrypt()
 *   wc_ChaCha20Poly1305_Decrypt()
 *   wc_HKDF_Expand()
 *
 * These are completely independent of the wolfSSL session state machine.
 */

#include "tlspeek_crypto.h"
#include "../common/tlspeek_log.h"
#include <wolfssl/options.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* wolfCrypt raw APIs */
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/kdf.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

/* ─────────────────────────────────────────────────────────────────────────── */

void tls13_compute_nonce(
    const uint8_t *iv,
    uint64_t       seq_num,
    uint8_t       *nonce_out)
{
    /* Start with the static IV */
    memcpy(nonce_out, iv, 12);

    /*
     * XOR the 8-byte big-endian seq_num into the last 8 bytes.
     * RFC 8446 §5.3: the 64-bit sequence number is left-padded to 12 bytes.
     */
    for (int i = 0; i < 8; i++) {
        nonce_out[11 - i] ^= (uint8_t)((seq_num >> (8 * i)) & 0xFF);
    }

    TLSPEEK_VLOG("[crypto] nonce computed for seq_num=%llu: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                 (unsigned long long)seq_num,
                 nonce_out[0],  nonce_out[1],  nonce_out[2],  nonce_out[3],
                 nonce_out[4],  nonce_out[5],  nonce_out[6],  nonce_out[7],
                 nonce_out[8],  nonce_out[9],  nonce_out[10], nonce_out[11]);
}

/* ─────────────────────────────────────────────────────────────────────────── */

int aead_aes_gcm_decrypt(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,        size_t aad_len,
    const uint8_t *ciphertext, size_t ct_len,
    const uint8_t *auth_tag,
    uint8_t       *plaintext)
{
    Aes aes;
    int ret;

    /* wc_AesGcmSetKey initialises the AES context with the key */
    ret = wc_AesGcmSetKey(&aes, key, 32 /* AES-256 */);
    if (ret != 0) {
        fprintf(stderr, "[crypto] wc_AesGcmSetKey failed: %d\n", ret);
        return -1;
    }

    /*
     * wc_AesGcmDecrypt() decrypts in place and verifies the auth tag.
     * Signature:
     *   wc_AesGcmDecrypt(Aes*, byte* out, const byte* in, word32 sz,
     *                    const byte* iv, word32 ivSz,
     *                    const byte* authTag, word32 authTagSz,
     *                    const byte* authIn, word32 authInSz)
     */
    ret = wc_AesGcmDecrypt(
        &aes,
        plaintext,          /* output */
        ciphertext,         /* input  */
        (word32)ct_len,     /* ciphertext length (without tag) */
        nonce, 12,          /* nonce / IV */
        auth_tag, 16,       /* auth tag */
        aad, (word32)aad_len /* additional authenticated data */
    );

    wc_AesFree(&aes);

    if (ret != 0) {
        fprintf(stderr,
                "[crypto] AES-GCM decryption/authentication failed: %d "
                "(BAD_FUNC_ARG=%d, AES_GCM_AUTH_E=%d)\n",
                ret, BAD_FUNC_ARG, AES_GCM_AUTH_E);
        return -1;
    }

    TLSPEEK_VLOG("[crypto] AES-GCM stateless decrypt OK, %zu plaintext bytes\n",
                 ct_len);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int aead_chacha20_poly1305_decrypt(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,        size_t aad_len,
    const uint8_t *ciphertext, size_t ct_len,
    const uint8_t *auth_tag,
    uint8_t       *plaintext)
{
    /*
     * wc_ChaCha20Poly1305_Decrypt() signature:
     *   int wc_ChaCha20Poly1305_Decrypt(
     *       const byte inKey[CHACHA20_POLY1305_AEAD_KEYSIZE],
     *       const byte inIV[CHACHA20_POLY1305_AEAD_IV_SIZE],
     *       const byte* inAAD, word32 inAADLen,
     *       const byte* inCiphertext, word32 inCiphertextLen,
     *       const byte inAuthTag[CHACHA20_POLY1305_AEAD_AUTHTAG_SIZE],
     *       byte* outPlaintext)
     *
     * Key must be exactly 32 bytes. Nonce must be exactly 12 bytes.
     */
    int ret = wc_ChaCha20Poly1305_Decrypt(
        key,
        nonce,
        aad,  (word32)aad_len,
        ciphertext, (word32)ct_len,
        auth_tag,
        plaintext
    );

    if (ret != 0) {
        fprintf(stderr,
                "[crypto] ChaCha20-Poly1305 decryption/authentication failed: %d\n",
                ret);
        return -1;
    }

    TLSPEEK_VLOG("[crypto] ChaCha20-Poly1305 stateless decrypt OK, %zu plaintext bytes\n",
                 ct_len);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int tls13_hkdf_expand_label(
    const uint8_t *secret,    size_t secret_len,
    const char    *label,
    const uint8_t *context,   size_t context_len,
    uint8_t       *out,       size_t out_len,
    int            hash_algo)
{
    /*
     * Build the HkdfLabel struct (RFC 8446 §7.1):
     *
     *   struct {
     *       uint16 length;                   // 2 bytes, big-endian
     *       opaque label<7..255>;            // 1-byte length + "tls13 " + label
     *       opaque context<0..255>;          // 1-byte length + context
     *   } HkdfLabel;
     *
     * Maximum sizes: 2 + (1 + 6 + 255) + (1 + 255) = 520 bytes → use 600.
     */
    uint8_t hkdf_label[600];
    size_t  pos = 0;

    /* length (uint16, big-endian) */
    hkdf_label[pos++] = (uint8_t)((out_len >> 8) & 0xFF);
    hkdf_label[pos++] = (uint8_t)( out_len       & 0xFF);

    /* label: 1-byte length prefix + "tls13 " + label */
    const char *prefix = "tls13 ";
    size_t prefix_len  = strlen(prefix);
    size_t label_len   = strlen(label);
    size_t full_label_len = prefix_len + label_len;
    hkdf_label[pos++] = (uint8_t)full_label_len;
    memcpy(hkdf_label + pos, prefix, prefix_len);
    pos += prefix_len;
    memcpy(hkdf_label + pos, label, label_len);
    pos += label_len;

    /* context: 1-byte length prefix + context */
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(hkdf_label + pos, context, context_len);
        pos += context_len;
    }

    TLSPEEK_VLOG("[crypto] HKDF-Expand-Label(label=\"tls13 %s\", out_len=%zu, hkdf_label_len=%zu, hash=%d)\n",
                 label, out_len, pos, hash_algo);

    /*
     * wc_HKDF_Expand() from wolfssl/wolfcrypt/kdf.h:
     *   int wc_HKDF_Expand(int type,
     *                      const byte* inKey, word32 inKeySz,
     *                      const byte* info,  word32 infoSz,
     *                      byte* out,         word32 outSz);
     */
    int ret = wc_HKDF_Expand(
        hash_algo,
        secret,  (word32)secret_len,
        hkdf_label, (word32)pos,
        out,  (word32)out_len
    );

    if (ret != 0) {
        fprintf(stderr, "[crypto] wc_HKDF_Expand failed: %d\n", ret);
        return -1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int hex_decode(const char *hex, size_t hex_len, uint8_t *out)
{
    if (hex_len % 2 != 0) {
        fprintf(stderr, "[crypto] hex_decode: odd hex string length %zu\n",
                hex_len);
        return -1;
    }

    for (size_t i = 0; i < hex_len; i += 2) {
        unsigned int byte;
        /* Use sscanf for portability — no format extension needed */
        char tmp[3] = { hex[i], hex[i + 1], '\0' };
        if (sscanf(tmp, "%02x", &byte) != 1) {
            fprintf(stderr, "[crypto] hex_decode: invalid hex char at pos %zu\n", i);
            return -1;
        }
        out[i / 2] = (uint8_t)byte;
    }

    return (int)(hex_len / 2);
}
