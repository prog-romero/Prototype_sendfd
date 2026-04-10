/*
 * config.h — MB-3.1 Configuration
 *
 * Paths, ports, and constants for TLS Migration vs Fresh Handshake benchmark
 */

#ifndef MB3_1_CONFIG_H
#define MB3_1_CONFIG_H

#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* PORTS & NETWORKING */
/* ─────────────────────────────────────────────────────────────────────────── */

#define GATEWAY_TLS_PORT            8443    /* Path A: TLS gateway (serialize + sendfd) */
#define WORKER_CLASSIC_PORT         9001    /* Path B: Fresh TLS handshake */

#define WORKER_MIGRATION_SOCKET     "/tmp/worker_migration.sock"

#define LOCALHOST                   "127.0.0.1"

/* ─────────────────────────────────────────────────────────────────────────── */
/* BENCHMARK CONFIGURATION */
/* ─────────────────────────────────────────────────────────────────────────── */

#define ITERATIONS_WARMUP 1
#define ITERATIONS_TOTAL 100

#define REQUEST_SIZE                256     /* Size of HTTP request */
#define RESPONSE_SIZE               256     /* Size of response */

/* ─────────────────────────────────────────────────────────────────────────── */
/* CERTIFICATE PATHS */
/* ─────────────────────────────────────────────────────────────────────────── */

#define CERT_DIR                    "../../../../libtlspeek/certs"
#define SERVER_CERT                 CERT_DIR "/server.crt"
#define SERVER_KEY                  CERT_DIR "/server.key"
#define CLIENT_CERT                 CERT_DIR "/client.crt"
#define CLIENT_KEY                  CERT_DIR "/client.key"

/* ─────────────────────────────────────────────────────────────────────────── */
/* LIBRARY PATHS */
/* ─────────────────────────────────────────────────────────────────────────── */

#define LIBTLSPEEK_INCDIR           "../../libtlspeek/lib"
#define LIBTLSPEEK_LIBDIR           "../../libtlspeek/build"
#define WOLFSSL_INCDIR              "../../wolfssl"
#define WOLFSSL_LIBDIR              "../../wolfssl/src/.libs"

/* ─────────────────────────────────────────────────────────────────────────── */
/* FILE PATHS */
/* ─────────────────────────────────────────────────────────────────────────── */

#define RESULTS_DIR                 "results"
#define RESULTS_CSV_COMBINED        "results/mb3_1_results.csv"
#define RESULTS_CSV_GATEWAY         "results/mb3_1_gateway_timings.csv"
#define RESULTS_CSV_WORKER          "results/mb3_1_worker_timings.csv"
#define RESULTS_CSV_HANDSHAKE       "results/mb3_1_handshake_timings.csv"

/* ─────────────────────────────────────────────────────────────────────────── */
/* TEST DATA */
/* ─────────────────────────────────────────────────────────────────────────── */

#define HTTP_REQUEST \
    "GET /hello HTTP/1.1\r\n" \
    "Host: localhost\r\n" \
    "Content-Length: 0\r\n" \
    "Connection: keep-alive\r\n" \
    "\r\n"

#define HTTP_RESPONSE \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: text/plain\r\n" \
    "Content-Length: 13\r\n" \
    "Connection: keep-alive\r\n" \
    "\r\n" \
    "Hello, World!"

/* ─────────────────────────────────────────────────────────────────────────── */
/* TIMING & STATISTICS */
/* ─────────────────────────────────────────────────────────────────────────── */

#define CLOCK_SOURCE                CLOCK_MONOTONIC_RAW

typedef struct {
    uint64_t migration_us;          /* TLS migration time (µs) */
    uint64_t handshake_us;          /* Fresh handshake time (µs) */
    uint32_t migration_ns;          /* Nanosecond component */
    uint32_t handshake_ns;          /* Nanosecond component */
} measurement_t;

#endif /* MB3_1_CONFIG_H */
