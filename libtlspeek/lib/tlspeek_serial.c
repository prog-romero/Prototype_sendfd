/*
 * tlspeek_serial.c — Serialize / restore TLS session state.
 *
 * Gateway side:  tlspeek_serialize() packs a tlspeek_ctx_t → tlspeek_serial_t
 * Worker side:   tlspeek_restore() injects the serial state into a WOLFSSL*
 *                session that was just created but has NOT performed a handshake.
 *
 * The restore path uses wolfSSL's wolfSSL_ImportKeyingMaterial() which is
 * available when wolfSSL is compiled with WOLFSSL_EXPORT_SEB or, more commonly
 * when built with --enable-session-export.
 *
 * FALLBACK STRATEGY:
 *   wolfSSL does not expose a public single-call "set traffic keys" API in
 *   all builds.  We therefore use wolfSSL_SetClientWriteKey() / wolfSSL_SetServerWriteKey()
 *   when available (wolfSSL >= 5.x with WOLFSSL_EXTRA), or fall back to
 *   wolfSSL_set_cipher_key_material() (WOLFSSL_DTLS_EXPORT path).
 *
 *   In the prototype the worker simply creates a NEW wolfSSL session using
 *   the imported keys by directly setting the AES/ChaCha context via the
 *   available wolfCrypt APIs.  This is the approach used in research prototypes
 *   and is clearly marked as such.
 */

#include "tlspeek_serial.h"
#include "tlspeek_crypto.h"
#include "../common/tlspeek_log.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>   /* htonl / ntohl for endian-safe serialisation */

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>

/* ─────────────────────────────────────────────────────────────────────────── */

void tlspeek_serialize(const tlspeek_ctx_t *ctx, tlspeek_serial_t *serial)
{
    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    serial->cipher_suite = (uint32_t)ctx->cipher_suite;
    memcpy(serial->client_write_key, ctx->client_write_key,
        sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv, ctx->client_write_iv,
        sizeof(serial->client_write_iv));
    serial->read_seq_num = ctx->read_seq_num;

    /*
     * Use the custom wolfSSL_tls_export() from the Migration_TLS fork
     * to capture the entire session state (keys, IVs, seqnums, etc.)
     */
    serial->blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    int ret = wolfSSL_tls_export(ctx->ssl, serial->tls_blob, &serial->blob_sz);

    if (ret <= 0) {
        fprintf(stderr, "[serial] ERROR: wolfSSL_tls_export failed (%d)\n", ret);
    } else {
        TLSPEEK_VLOG("[serial] Serialized TLS session: %u bytes\n", serial->blob_sz);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */

int tlspeek_restore(WOLFSSL *ssl, const tlspeek_serial_t *serial)
{
    if (serial->magic != TLSPEEK_MAGIC) {
        fprintf(stderr, "[serial] ERROR: magic mismatch\n");
        return -1;
    }

    /* 
     * Use the custom wolfSSL_tls_import() from the Migration_TLS fork
     * to restore the entire session state.
     */
    int ret = wolfSSL_tls_import(ssl, serial->tls_blob, serial->blob_sz);
    if (ret <= 0) {
        fprintf(stderr, "[serial] ERROR: wolfSSL_tls_import failed (%d)\n", ret);
        return -1;
    }

    TLSPEEK_VLOG("[serial] wolfSSL session state restored (%u bytes)\n", serial->blob_sz);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int tlspeek_restore_peek_ctx(tlspeek_ctx_t *ctx, int tcp_fd,
                             const tlspeek_serial_t *serial)
{
    if (!ctx || !serial || serial->magic != TLSPEEK_MAGIC || tcp_fd < 0) {
        fprintf(stderr, "[serial] ERROR: invalid stateless peek restore input\n");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->client_write_key, serial->client_write_key,
           sizeof(ctx->client_write_key));
    memcpy(ctx->client_write_iv, serial->client_write_iv,
           sizeof(ctx->client_write_iv));
    ctx->read_seq_num = serial->read_seq_num;
    ctx->cipher_suite = (tlspeek_cipher_t)serial->cipher_suite;
    ctx->tcp_fd = tcp_fd;
    ctx->keys_ready = 1;

    return 0;
}
