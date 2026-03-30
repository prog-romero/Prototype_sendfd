#ifndef TLSPEEK_CRYPTO_H
#define TLSPEEK_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/*
 * tlspeek_crypto.h — Internal stateless AEAD decryption primitives.
 *
 * These functions do NOT interact with wolfSSL's TLS state machine.
 * They call wolfCrypt low-level APIs directly (wc_AesGcmDecrypt,
 * wc_ChaCha20Poly1305_Decrypt, wc_HKDF_Expand).
 */

/* ── AEAD decryption ──────────────────────────────────────────────────────── */

/**
 * aead_aes_gcm_decrypt() — Stateless AES-256-GCM decryption.
 *
 * @param key         32-byte AES-256 key (client_write_key).
 * @param nonce       12-byte nonce (IV XOR seq_num).
 * @param aad         Additional authenticated data (TLS record header, 5 B).
 * @param aad_len     Length of aad (always 5 for TLS 1.3).
 * @param ciphertext  Encrypted payload buffer.
 * @param ct_len      Length of ciphertext (excluding auth tag).
 * @param auth_tag    16-byte authentication tag.
 * @param plaintext   Output buffer (must be >= ct_len bytes).
 * @return 0 on success, -1 on authentication failure or error.
 */
int aead_aes_gcm_decrypt(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,     size_t aad_len,
    const uint8_t *ciphertext, size_t ct_len,
    const uint8_t *auth_tag,
    uint8_t       *plaintext
);

/**
 * aead_chacha20_poly1305_decrypt() — Stateless ChaCha20-Poly1305 decryption.
 *
 * Same parameter convention as above.
 */
int aead_chacha20_poly1305_decrypt(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,     size_t aad_len,
    const uint8_t *ciphertext, size_t ct_len,
    const uint8_t *auth_tag,
    uint8_t       *plaintext
);

/* ── TLS 1.3 nonce computation ────────────────────────────────────────────── */

/**
 * tls13_compute_nonce() — XOR the static IV with the sequence number.
 *
 * From RFC 8446 §5.3:
 *   nonce = write_iv XOR seq_num_padded
 * where seq_num_padded is the 8-byte big-endian seq_num left-padded to 12 B.
 *
 * @param iv        12-byte static IV (client_write_iv or server_write_iv).
 * @param seq_num   Current record sequence number.
 * @param nonce_out 12-byte output nonce buffer.
 */
void tls13_compute_nonce(
    const uint8_t *iv,
    uint64_t       seq_num,
    uint8_t       *nonce_out
);

/* ── HKDF-Expand-Label (RFC 8446 §7.1) ───────────────────────────────────── */

/**
 * tls13_hkdf_expand_label() — Derive a key or IV from a traffic secret.
 *
 * HkdfLabel = { uint16 length, opaque label<7..255>, opaque context<0..255> }
 * label is prepended with "tls13 ".
 *
 * @param secret      Input keying material (hex-decoded traffic secret).
 * @param secret_len  Length of secret.
 * @param label       Label string (e.g. "key" or "iv") — WITHOUT "tls13 " prefix.
 * @param context     Context bytes (empty for TLS 1.3 key/iv derivation).
 * @param context_len Length of context (0 for key/iv).
 * @param out         Output buffer.
 * @param out_len     Desired output length (32 for key, 12 for iv).
 * @param hash_algo   WC_SHA384 or WC_SHA256 depending on cipher suite.
 * @return 0 on success, -1 on error.
 */
int tls13_hkdf_expand_label(
    const uint8_t *secret,   size_t secret_len,
    const char    *label,
    const uint8_t *context,  size_t context_len,
    uint8_t       *out,      size_t out_len,
    int            hash_algo
);

/* ── Hex decoding helper ──────────────────────────────────────────────────── */

/**
 * hex_decode() — Convert a hex string to raw bytes.
 *
 * @param hex      Input hex string (lowercase, no spaces).
 * @param hex_len  Length of hex string (must be even).
 * @param out      Output buffer (must be >= hex_len / 2 bytes).
 * @return number of bytes written, or -1 on invalid input.
 */
int hex_decode(const char *hex, size_t hex_len, uint8_t *out);

#endif /* TLSPEEK_CRYPTO_H */
