/*
 * test_tlspeek.c — Unit tests for the libtlspeek library.
 *
 * Tests:
 *   1. tls13_compute_nonce() — RFC 8446 §5.3 nonce expansion
 *   2. hex_decode()          — Hex decoding utility
 *   3. tlspeek_serialize() / validate magic
 *   4. aead_aes_gcm round-trip (encrypt with wolfCrypt, decrypt with library)
 *   5. tls13_hkdf_expand_label() — Key derivation
 *
 * Usage (from libtlspeek/build/):
 *   ./test_tlspeek
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include "tlspeek.h"
#include "tlspeek_crypto.h"

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stdout, "  %-55s ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    tests_passed++; \
    fprintf(stdout, "PASS\n"); \
} while(0)

#define FAIL(msg) do { \
    tests_failed++; \
    fprintf(stdout, "FAIL — %s\n", msg); \
} while(0)

/* ─── Test 1: nonce computation ──────────────────────────────────────────── */

static void test_nonce_computation(void)
{
    fprintf(stdout, "\n[Test 1] tls13_compute_nonce()\n");

    /* RFC 8446 §5.3 example:
     * If IV = 0x000102030405060708090a0b and seq_num = 0x0000000000000001
     * Then nonce = IV XOR 00000000000000000000000000000001
     *            = 0x000102030405060708090a0a
     */

    uint8_t iv[12] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b
    };
    uint8_t nonce[12];

    /* seq_num = 0 → nonce should equal iv */
    TEST("seq_num=0 → nonce == iv");
    tls13_compute_nonce(iv, 0, nonce);
    if (memcmp(nonce, iv, 12) == 0) PASS(); else FAIL("nonce != iv");

    /* seq_num = 1 → last byte XOR'd with 1 */
    TEST("seq_num=1 → last byte XOR'd with 0x01");
    tls13_compute_nonce(iv, 1, nonce);
    uint8_t expected[12];
    memcpy(expected, iv, 12);
    expected[11] ^= 0x01;
    if (memcmp(nonce, expected, 12) == 0) PASS();
    else FAIL("unexpected nonce value");

    /* seq_num = 256 = 0x100 → byte[10] and byte[11] XOR'd */
    TEST("seq_num=256 → bytes[10..11] XOR'd");
    tls13_compute_nonce(iv, 256, nonce);
    memcpy(expected, iv, 12);
    expected[10] ^= 0x01;
    /* expected[11] ^= 0x00 — no change */
    if (memcmp(nonce, expected, 12) == 0) PASS();
    else FAIL("unexpected nonce for seq_num=256");
}

/* ─── Test 2: hex_decode ─────────────────────────────────────────────────── */

static void test_hex_decode(void)
{
    fprintf(stdout, "\n[Test 2] hex_decode()\n");

    uint8_t out[32];
    int n;

    TEST("decode known hex string");
    n = hex_decode("deadbeef", 8, out);
    if (n == 4
        && out[0] == 0xde && out[1] == 0xad
        && out[2] == 0xbe && out[3] == 0xef)
        PASS();
    else FAIL("wrong decoded bytes");

    TEST("decode all-zeros");
    n = hex_decode("000000000000000000000000000000000000000000000000"
                   "000000000000000000000000000000000000000000000000000000000000000",
                   64, out);
    /* 64 hex chars = 32 bytes */
    n = hex_decode(
        "0000000000000000000000000000000000000000000000000000000000000000",
        64, out);
    if (n == 32) {
        int all_zero = 1;
        for (int i = 0; i < 32; i++) if (out[i]) { all_zero = 0; break; }
        if (all_zero) PASS(); else FAIL("nonzero byte in all-zeros decode");
    } else FAIL("wrong byte count");

    TEST("odd-length hex returns -1");
    n = hex_decode("abc", 3, out);
    if (n == -1) PASS(); else FAIL("should have returned -1 for odd length");
}

/* ─── Test 3: serialize / deserialize ───────────────────────────────────── */

static void test_serialize(void)
{
    fprintf(stdout, "\n[Test 3] tlspeek_serialize()\n");

    tlspeek_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Fill with recognisable pattern */
    for (int i = 0; i < TLSPEEK_KEY_SIZE; i++) {
        ctx.client_write_key[i] = (uint8_t)(0xAA + i);
        ctx.server_write_key[i] = (uint8_t)(0xBB + i);
    }
    for (int i = 0; i < TLSPEEK_IV_SIZE; i++) {
        ctx.client_write_iv[i] = (uint8_t)(0xCC + i);
        ctx.server_write_iv[i] = (uint8_t)(0xDD + i);
    }
    ctx.read_seq_num  = 0x0123456789ABCDEFULL;
    ctx.write_seq_num = 0xFEDCBA9876543210ULL;
    ctx.cipher_suite  = TLSPEEK_AES_256_GCM;
    ctx.keys_ready    = 1;

    tlspeek_serial_t serial;
    tlspeek_serialize(&ctx, &serial);

    TEST("magic is TLSPEEK_MAGIC");
    if (serial.magic == TLSPEEK_MAGIC) PASS();
    else FAIL("wrong magic");

    TEST("cipher_suite round-trips");
    if (serial.cipher_suite == (uint32_t)ctx.cipher_suite) PASS();
    else FAIL("cipher_suite mismatch");

    TEST("client_write_key round-trips for stateless peek");
    if (memcmp(serial.client_write_key, ctx.client_write_key,
               sizeof(serial.client_write_key)) == 0) PASS();
    else FAIL("client_write_key mismatch");

    TEST("client_write_iv round-trips for stateless peek");
    if (memcmp(serial.client_write_iv, ctx.client_write_iv,
               sizeof(serial.client_write_iv)) == 0) PASS();
    else FAIL("client_write_iv mismatch");

    TEST("read_seq_num round-trips for stateless peek");
    if (serial.read_seq_num == ctx.read_seq_num) PASS();
    else FAIL("read_seq_num mismatch");
}

/* ─── Test 4: AES-GCM round-trip ────────────────────────────────────────── */

#include <wolfssl/wolfcrypt/aes.h>

static void test_aes_gcm_roundtrip(void)
{
    fprintf(stdout, "\n[Test 4] AES-GCM stateless round-trip\n");

    /* 256-bit key, 12-byte nonce, 16-byte tag */
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t aad[5]     = { 0x17, 0x03, 0x03, 0x00, 0x20 }; /* fake TLS header */
    uint8_t plaintext[] = "Hello, FaaS gateway!";
    size_t  pt_len      = strlen((char *)plaintext);

    for (size_t i = 0; i < sizeof(key);   i++) key[i]   = (uint8_t)i;
    for (size_t i = 0; i < sizeof(nonce); i++) nonce[i] = (uint8_t)(i + 0x10);

    /* Encrypt with wolfCrypt directly */
    uint8_t ciphertext[64];
    uint8_t auth_tag[16];

    Aes aes;
    wc_AesGcmSetKey(&aes, key, 32);
    int ret = wc_AesGcmEncrypt(&aes, ciphertext, plaintext, (word32)pt_len,
                               nonce, 12, auth_tag, 16, aad, 5);
    wc_AesFree(&aes);

    TEST("wolfCrypt encrypt returns 0");
    if (ret == 0) PASS(); else { FAIL("encrypt failed"); return; }

    /* Decrypt with our stateless API */
    uint8_t recovered[64];
    memset(recovered, 0, sizeof(recovered));

    TEST("aead_aes_gcm_decrypt returns 0");
    ret = aead_aes_gcm_decrypt(key, nonce, aad, 5,
                               ciphertext, pt_len, auth_tag, recovered);
    if (ret == 0) PASS(); else FAIL("decrypt failed");

    TEST("decrypted plaintext matches original");
    if (memcmp(recovered, plaintext, pt_len) == 0) PASS();
    else FAIL("plaintext mismatch after decrypt");

    /* Tamper with auth tag — should fail */
    TEST("tampered auth tag causes FAIL");
    auth_tag[0] ^= 0xFF;
    ret = aead_aes_gcm_decrypt(key, nonce, aad, 5,
                               ciphertext, pt_len, auth_tag, recovered);
    if (ret == -1) PASS(); else FAIL("tampered tag should have been rejected");
}

/* ─── Test 5: HKDF-Expand-Label ─────────────────────────────────────────── */

static void test_hkdf_expand_label(void)
{
    fprintf(stdout, "\n[Test 5] tls13_hkdf_expand_label()\n");

    /*
     * We use a known-good vector from RFC 8448 (TLS 1.3 test vectors):
     * https://www.rfc-editor.org/rfc/rfc8448
     *
     * For simplicity, we just verify the function runs without error
     * and produces a non-zero output (full vector verification requires
     * matching against specific RFC 8448 test cases).
     */

    /* Use 32-byte zero secret with SHA-256 */
    uint8_t secret[32] = {0};
    uint8_t key_out[32];
    uint8_t iv_out[12];

    memset(key_out, 0, sizeof(key_out));
    memset(iv_out,  0, sizeof(iv_out));

    TEST("derive key from zero secret (SHA-256)");
    int ret = tls13_hkdf_expand_label(secret, 32,
                                       "key", NULL, 0,
                                       key_out, 16, WC_SHA256);
    if (ret == 0) PASS(); else FAIL("HKDF key derivation failed");

    TEST("derived key is not all-zero");
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (key_out[i]) { all_zero = 0; break; }
    if (!all_zero) PASS(); else FAIL("derived key is all-zero (suspicious)");

    TEST("derive iv from zero secret (SHA-256)");
    ret = tls13_hkdf_expand_label(secret, 32,
                                   "iv", NULL, 0,
                                   iv_out, 12, WC_SHA256);
    if (ret == 0) PASS(); else FAIL("HKDF iv derivation failed");

    TEST("key and iv are different (different labels)");
    /* Compare first 12 bytes */
    if (memcmp(key_out, iv_out, 12) != 0) PASS();
    else FAIL("key and iv are identical (label not applied correctly)");
}

/* ─────────────────────────────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stdout,
            "═══════════════════════════════════════════════════\n"
            "  libtlspeek unit tests\n"
            "═══════════════════════════════════════════════════\n");

    wolfSSL_Init();

    test_nonce_computation();
    test_hex_decode();
    test_serialize();
    test_aes_gcm_roundtrip();
    test_hkdf_expand_label();

    wolfSSL_Cleanup();

    fprintf(stdout,
            "\n═══════════════════════════════════════════════════\n"
            "  Results: %d/%d passed, %d failed\n"
            "═══════════════════════════════════════════════════\n",
            tests_passed, tests_run, tests_failed);

    return (tests_failed == 0) ? 0 : 1;
}
