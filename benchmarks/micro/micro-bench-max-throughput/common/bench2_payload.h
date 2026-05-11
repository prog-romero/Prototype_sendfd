/*
 * bench2_payload.h — Wire payload sent alongside a raw TCP fd when the fd
 * is transferred between processes in the benchmark-2 (keepalive) prototype.
 *
 * Requires wolfSSL + libtlspeek headers (for tlspeek_serial_t).
 * Include AFTER wolfssl/options.h or after defining HAVE_SECRET_CALLBACK.
 *
 * Both the proto gateway C shim (bench2gw.c) and the proto worker
 * (bench2_proto_worker.c) include this header so the struct layout is
 * guaranteed to match across the sendfd_with_state() boundary.
 */
#ifndef BENCH2_PAYLOAD_H
#define BENCH2_PAYLOAD_H

#ifndef HAVE_SECRET_CALLBACK
#  define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#  define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <tlspeek/tlspeek.h>
#include <stdint.h>

#define BENCH2_KA_MAGIC      0x42324B41U   /* 'B2KA' */
#define BENCH2_KA_VERSION    1U
#define BENCH2_KA_TARGET_LEN 128

/*
 * bench2_keepalive_payload_t
 *
 * Carried alongside the raw TCP fd when transferred via sendfd_with_state().
 *
 * Fields:
 *   magic          — BENCH2_KA_MAGIC, sanity check on reception.
 *   version        — BENCH2_KA_VERSION.
 *   serial         — TLS session state (keys, IV, seq-num, wolfSSL export blob).
 *   target_function— null-terminated name of the function that should handle
 *                    the next request on this connection.
 */
typedef struct {
    uint32_t         magic;
    uint32_t         version;
    tlspeek_serial_t serial;
    char             target_function[BENCH2_KA_TARGET_LEN];
} bench2_keepalive_payload_t;

#endif /* BENCH2_PAYLOAD_H */
