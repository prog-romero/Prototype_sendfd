/*
 * bench2_proto_worker.c — Benchmark-2 keepalive prototype container worker.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * Timing discipline (rdtsc / CNTVCT_EL0):
 *
 *  REQUEST 1 — first time this container receives the live TLS fd
 *  ──────────────────────────────────────────────────────────────
 *   top1 was stamped AT THE GATEWAY (bench2gw.c) just before tls_read_peek.
 *   It is carried in payload.top1_rdtsc / cntfrq / top1_set.
 *   Here the worker:
 *     1. Restores the TLS session from payload.serial.
 *     2. Reads the full HTTP request body via wolfSSL_read.
 *   top2 = bench2_rdtsc() after wolfSSL_read completes.
 *   delta = top2 – top1  (cycles, same globally-synchronised counter on arm64)
 *
 *  REQUEST 2+ — container already holds the live fd from a previous request
 *  ──────────────────────────────────────────────────────────────────────────
 *   SUB-CASE A: request belongs to THIS container
 *     bench2_wait_readable(tcp_fd)   — wait for encrypted bytes
 *     top1 = bench2_rdtsc()          — stamp before peek
 *     tls_read_peek(...)             — identify owner
 *     → owner matches → wolfSSL_read full request
 *     top2 = bench2_rdtsc()          — after full read
 *
 *   SUB-CASE B: request belongs to the OTHER container
 *     bench2_wait_readable(tcp_fd)   — wait for encrypted bytes
 *     top1 = bench2_rdtsc()          — stamp before peek
 *     tls_read_peek(...)             — identify owner → NOT us
 *     → pack top1 into bench2_keepalive_payload_t
 *     → export TLS state (wolfSSL_tls_export + atomic-user keys)
 *     → sendfd_with_state to relay socket
 *     top2 is stamped by the correct owner when it eventually reads.
 *     (The correct owner receives the fd + payload.top1_rdtsc and uses it.)
 *
 * ──────────────────────────────────────────────────────────────────────────
 * Environment variables:
 *   BENCH2_FUNCTION_NAME    function name (e.g. "bench2-fn-a")
 *   BENCH2_SOCKET_DIR       directory for per-function sockets (default /run/bench2)
 *   BENCH2_RELAY_SOCKET     gateway relay socket path
 *   BENCH2_CERT             TLS certificate (default /certs/server.crt)
 *   BENCH2_KEY              TLS private key  (default /certs/server.key)
 * ──────────────────────────────────────────────────────────────────────────
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HAVE_SECRET_CALLBACK
#  define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#  define WOLFSSL_KEYLOG_EXPORT
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <tlspeek/tlspeek.h>
#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

#include "bench2_rdtsc.h"
#include "bench2_payload.h"

#ifdef BENCH2_WORKER_ENABLE_TRACE
#define BENCH2_WORKER_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define BENCH2_WORKER_TRACE(...) ((void)0)
#endif

/* ─── HTTP parsing helpers ──────────────────────────────────────────────── */

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0)
            return (ssize_t)i;
    return -1;
}

static long long parse_header_int64(const char *hdr, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p = hdr, *end = hdr + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll >= nlen) {
            int m = 1;
            for (size_t k = 0; k < nlen && m; k++) {
                char c = p[k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == needle[k]);
            }
            if (m) {
                const char *v = p + nlen;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                char *ep = NULL;
                long long val = strtoll(v, &ep, 10);
                if (ep && ep > v) return val;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return -1;
}

static long long parse_content_length(const char *hdr, size_t hlen)
{
    return parse_header_int64(hdr, hlen, "content-length:");
}

static int has_connection_close(const char *hdr, size_t hlen)
{
    const char *p = hdr, *end = hdr + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char key[] = "connection:";
        size_t kl = sizeof(key) - 1;
        if (ll >= kl) {
            int m = 1;
            for (size_t k = 0; k < kl && m; k++) {
                char c = p[k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == key[k]);
            }
            if (m) {
                const char *v = p + kl, *ve = nl ? nl : end;
                while (v + 4 < ve) {
                    if ((v[0]=='c'||v[0]=='C') && (v[1]=='l'||v[1]=='L') &&
                        (v[2]=='o'||v[2]=='O') && (v[3]=='s'||v[3]=='S') &&
                        (v[4]=='e'||v[4]=='E')) return 1;
                    v++;
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    return 0;
}

static int has_single_owner_hint(const char *hdr, size_t hlen)
{
    return parse_header_int64(hdr, hlen, "x-bench2-single-owner:") > 0;
}

/* Parse function name from HTTP request line "METHOD /function/<name> HTTP..." */
static bool parse_request_owner(const unsigned char *buf, size_t len,
                                char *out, size_t outsz)
{
    if (!buf || !len || !out || !outsz) return false;
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
        memcmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++)
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') { nlen = i; break; }
    if (!nlen || nlen + 1 > outsz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

static int peek_owner_from_kernel(int client_fd, const tlspeek_serial_t *serial,
                                  char *owner_out, size_t owner_out_sz)
{
    unsigned char peek_buf[8193];
    size_t want = sizeof(peek_buf) - 1;
    if (want > 1024) want = 1024;

    while (want > 0) {
        tlspeek_ctx_t peek_ctx;
        if (tlspeek_restore_peek_ctx(&peek_ctx, client_fd, serial) != 0) {
            return -1;
        }

        int peeked = tls_read_peek(&peek_ctx, peek_buf, want);
        tlspeek_free(&peek_ctx);
        if (peeked <= 0) {
            return -1;
        }

        peek_buf[peeked] = '\0';
        if (parse_request_owner(peek_buf, (size_t)peeked, owner_out, owner_out_sz)) {
            return 0;
        }

        if (want >= sizeof(peek_buf) - 1) break;
        want *= 2;
        if (want > sizeof(peek_buf) - 1) want = sizeof(peek_buf) - 1;
    }

    return -1;
}

/* ─── TLS helpers ───────────────────────────────────────────────────────── */

static void set_serial_cipher_from_name(tlspeek_serial_t *serial, const char *name)
{
    if (!serial) return;
    if (!name) {
        serial->cipher_suite = TLSPEEK_AES_256_GCM;
        return;
    }

    if (strstr(name, "CHACHA20"))
        serial->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES-128"))
        serial->cipher_suite = TLSPEEK_AES_128_GCM;
    else
        serial->cipher_suite = TLSPEEK_AES_256_GCM;
}

static int wolfssl_write_all(WOLFSSL *ssl, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = wolfSSL_write(ssl, p, (int)len);
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* wait_for_next_tls_bytes — block with epoll until fd is readable.
 * Returns 1 if data available, 0 if peer closed, -1 on error.
 * Uses epoll_wait (no busy-spin, no poll). */
static int wait_for_next_tls_bytes(int fd)
{
    int epfd = epoll_create1(0);
    if (epfd < 0) return -1;

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        close(epfd);
        return -1;
    }

    int result = -1;
    struct epoll_event out;
    for (;;) {
        int n = epoll_wait(epfd, &out, 1, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (n == 0) continue;  /* spurious wakeup */

        if (out.events & (EPOLLRDHUP | EPOLLHUP)) {
            result = 0;  /* peer closed */
            break;
        }
        if (out.events & EPOLLERR) {
            result = -1;
            break;
        }
        if (out.events & EPOLLIN) {
            result = 1;
            break;
        }
    }
    close(epfd);
    return result;
}

/* Export current live wolfSSL session key material into a serial struct.
 *
 * tls_blob (full session export) is needed for relay to another container.
 * tlspeek_restore_peek_ctx only needs client_write_key/iv + read_seq_num +
 * cipher_suite, so a failed wolfSSL_tls_export is non-fatal: self-peek still
 * works; only cross-container relay would be broken in that case.
 */
static int export_live_serial(WOLFSSL *ssl, tlspeek_serial_t *serial)
{
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    const unsigned char *key, *iv;
    int key_sz, iv_sz;
    word64 peer_seq = 0;

    if (!ssl || !serial) return -1;
    serial->magic = TLSPEEK_MAGIC;
    set_serial_cipher_from_name(serial, wolfSSL_get_cipher_name(ssl));

    /* Non-fatal: blob is only required for relay, not for local peek. */
    int rc = wolfSSL_tls_export(ssl, serial->tls_blob, &blob_sz);
    if (rc > 0)
        serial->blob_sz = blob_sz;
    else
        serial->blob_sz = 0; /* relay won't work but local peek still will */

    key    = wolfSSL_GetClientWriteKey(ssl);
    iv     = wolfSSL_GetClientWriteIV(ssl);
    key_sz = wolfSSL_GetKeySize(ssl);
    iv_sz  = wolfSSL_GetIVSize(ssl);
    if (!key || !iv || key_sz <= 0 || iv_sz <= 0) return -1;

    size_t ks = (size_t)key_sz < sizeof(serial->client_write_key)
                ? (size_t)key_sz : sizeof(serial->client_write_key);
    size_t is = (size_t)iv_sz  < sizeof(serial->client_write_iv)
                ? (size_t)iv_sz  : sizeof(serial->client_write_iv);
    memcpy(serial->client_write_key, key, ks);
    memcpy(serial->client_write_iv,  iv,  is);

    if (wolfSSL_GetPeerSequenceNumber(ssl, &peer_seq) < 0) return -1;
    serial->read_seq_num = (uint64_t)peer_seq;
    BENCH2_WORKER_TRACE("[bench2-worker] export_live_serial cipher=0x%04x name=%s seq=%" PRIu64 "\n",
                        (unsigned int)serial->cipher_suite,
                        wolfSSL_get_cipher_name(ssl) ? wolfSSL_get_cipher_name(ssl) : "<null>",
                        serial->read_seq_num);
    return 0;
}

/* ─── Send session back to relay socket ─────────────────────────────────── */

static int relay_to_gateway(int client_fd,
                            bench2_keepalive_payload_t *payload,
                            const char *relay_socket)
{
    int relay_fd = unix_client_connect(relay_socket);
    if (relay_fd < 0) { close(client_fd); return -1; }

    int rc = sendfd_with_state(relay_fd, client_fd, payload, sizeof(*payload));
    close(relay_fd);
    if (rc != 0) { close(client_fd); return -1; }
    return 0;
}

/* ─── Send HTTP response ─────────────────────────────────────────────────── */

static int send_response(WOLFSSL *ssl,
                         const char *worker_name,
                         uint64_t req_no,
                         uint64_t top1, uint64_t top2,
                         uint64_t top_container_before_read,
                         uint64_t cntfrq,
                         int should_close)
{
    uint64_t delta = (top2 > top1) ? top2 - top1 : 0;
    uint64_t delta_migration_ns = (top_container_before_read > top1)
                                  ? top_container_before_read - top1 : 0;

    char json[640];
    int jlen = snprintf(json, sizeof(json),
        "{\"worker\":\"%s\",\"request_no\":%" PRIu64
        ",\"path\":\"prototype\""
        ",\"top1_rdtsc\":%" PRIu64
        ",\"top2_rdtsc\":%" PRIu64
        ",\"delta_cycles\":%" PRIu64
        ",\"cntfrq\":%" PRIu64
        ",\"top_container_before_read\":%" PRIu64
        ",\"delta_migration_ns\":%" PRIu64 "}\n",
        worker_name, req_no,
        top1, top2, delta, cntfrq,
        top_container_before_read, delta_migration_ns);
    if (jlen <= 0 || (size_t)jlen >= sizeof(json)) return -1;

    char resp[768];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        jlen,
        should_close ? "close" : "keep-alive",
        json);
    if (rlen <= 0 || (size_t)rlen >= sizeof(resp)) return -1;
    return wolfssl_write_all(ssl, resp, (size_t)rlen);
}

static int read_full_request(WOLFSSL *ssl,
                             uint64_t *top2_out,
                             int *req_close_out,
                             int *single_owner_hint_out,
                             char *owner_out,
                             size_t owner_out_sz)
{
    unsigned char rbuf[65536 + 1];
    size_t rlen = 0;
    ssize_t hdr_end = -1;

    while (hdr_end < 0) {
        int n = wolfSSL_read(ssl, rbuf + rlen, (int)(sizeof(rbuf) - 1 - rlen));
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
                continue;
            return -1;
        }
        rlen += (size_t)n;
        hdr_end = find_subseq(rbuf, rlen, "\r\n\r\n");
    }

    size_t hdr_sz = (size_t)hdr_end + 4;
    long long cl = parse_content_length((const char *)rbuf, hdr_sz);
    if (cl < 0) cl = 0;

    if (req_close_out)
        *req_close_out = has_connection_close((const char *)rbuf, hdr_sz);
    if (single_owner_hint_out)
        *single_owner_hint_out = has_single_owner_hint((const char *)rbuf, hdr_sz);

    if (owner_out && owner_out_sz > 0) {
        if (!parse_request_owner(rbuf, hdr_sz, owner_out, owner_out_sz))
            owner_out[0] = '\0';
    }

    size_t body_in = (rlen > hdr_sz) ? rlen - hdr_sz : 0;
    while (body_in < (size_t)cl) {
        unsigned char scratch[8192];
        size_t want = (size_t)cl - body_in;
        if (want > sizeof(scratch)) want = sizeof(scratch);
        int n = wolfSSL_read(ssl, scratch, (int)want);
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
                continue;
            return -1;
        }
        body_in += (size_t)n;
    }

    if (top2_out)
        *top2_out = bench2_rdtsc();
    return 0;
}

/* ─── process_session: main per-connection loop ─────────────────────────── */

static void process_session(WOLFSSL_CTX *wctx,
                            int client_fd,
                            bench2_keepalive_payload_t *payload,
                            const char *function_name,
                            const char *relay_socket)
{
    const char *fail_stage = NULL;
    WOLFSSL *ssl = wolfSSL_new(wctx);
    if (!ssl) { close(client_fd); return; }

    wolfSSL_set_fd(ssl, client_fd);
    if (tlspeek_restore(ssl, &payload->serial) != 0) {
        fail_stage = "tlspeek_restore";
        goto done;
    }
    wolfSSL_set_fd(ssl, client_fd);

    BENCH2_WORKER_TRACE("[bench2-worker] received session fn=%s target=%s top1_set=%u cipher=0x%04x seq=%" PRIu64 "\n",
                        function_name,
                        payload->target_function[0] ? payload->target_function : "<unset>",
                        (unsigned int)payload->top1_set,
                        (unsigned int)payload->serial.cipher_suite,
                        payload->serial.read_seq_num);

    uint64_t req_no     = 0;
    bool     first_recv = true;   /* true when we receive fd from gateway */
    bool     single_owner_direct_read = false;

    for (;;) {
        req_no++;

        uint64_t top1 = 0, top2 = 0, cntfrq_stamp = 0;

        if (first_recv && payload->top1_set) {
            /*
             * Request 1 (or relay Case 3): top1 was stamped at the gateway
             * or at the wrong worker before relay. We have it in the payload.
             * TLS state is fully restored; stamp top_container_before_read
             * just before the first real wolfSSL_read.
             */
            top1         = payload->top1_rdtsc;
            cntfrq_stamp = payload->cntfrq;
            first_recv   = false;

            uint64_t top_cbr = bench2_rdtsc(); /* top_container_before_read */

            int req_close = 0;
            int single_owner_hint = 0;
            if (read_full_request(ssl, &top2, &req_close, &single_owner_hint,
                                  NULL, 0) != 0) {
                fail_stage = "first_read_full_request";
                goto done;
            }
            single_owner_direct_read = single_owner_hint != 0;

            if (send_response(ssl, function_name, req_no,
                              top1, top2, top_cbr, cntfrq_stamp, req_close) != 0) {
                fail_stage = "first_send_response";
                goto done;
            }
            if (req_close) { break; }

        } else {
            /*
             * Request 2+: we own the live fd.
             * Wait for data, stamp top1, peek to determine owner.
             */
            int wait_rc = wait_for_next_tls_bytes(client_fd);
            if (wait_rc < 0) {
                fail_stage = "wait_for_next_tls_bytes";
                goto done;
            }
            if (wait_rc == 0) {
                BENCH2_WORKER_TRACE("[bench2-worker] peer closed session fn=%s after req=%" PRIu64 "\n",
                            function_name,
                            req_no - 1);
                break;
            }

            top1         = bench2_rdtsc();
            cntfrq_stamp = bench2_cntfrq();

            if (single_owner_direct_read) {
                /* Case 4: single-owner fast path — no migration, no peek.
                 * top_container_before_read = 0 (not applicable). */
                int req_close = 0;
                char owner[BENCH2_KA_TARGET_LEN];

                if (read_full_request(ssl, &top2, &req_close, NULL,
                                      owner, sizeof(owner)) != 0) {
                    fail_stage = "next_read_direct_full_request";
                    goto done;
                }
                if (owner[0] == '\0' || strcmp(owner, function_name) != 0) {
                    fail_stage = "next_read_direct_owner_mismatch";
                    goto done;
                }

                if (send_response(ssl, function_name, req_no,
                                  top1, top2, 0, cntfrq_stamp, req_close) != 0) {
                    fail_stage = "next_send_direct_response";
                    goto done;
                }
                if (req_close) break;
                if (export_live_serial(ssl, &payload->serial) != 0) {
                    fail_stage = "export_live_serial";
                    goto done;
                }
                continue;
            }

            char owner[BENCH2_KA_TARGET_LEN];
            BENCH2_WORKER_TRACE("[bench2-worker] before peek req=%" PRIu64 " cipher=0x%04x seq=%" PRIu64 "\n",
                    req_no,
                    (unsigned int)payload->serial.cipher_suite,
                    payload->serial.read_seq_num);
            if (peek_owner_from_kernel(client_fd, &payload->serial,
                                       owner, sizeof(owner)) != 0) {
                fail_stage = "peek_owner_from_kernel";
                goto done;
            }

            if (strcmp(owner, function_name) != 0) {
                /*
                 * Sub-case B: wrong owner.
                 * Pack top1 and the previously exported TLS state,
                 * send to relay socket.
                 */
                bench2_keepalive_payload_t relay_payload;
                memset(&relay_payload, 0, sizeof(relay_payload));
                relay_payload.magic      = BENCH2_KA_MAGIC;
                relay_payload.version    = BENCH2_KA_VERSION;
                relay_payload.top1_rdtsc = top1;
                relay_payload.cntfrq     = cntfrq_stamp;
                relay_payload.top1_set   = 1;
                memcpy(&relay_payload.serial, &payload->serial, sizeof(payload->serial));
                snprintf(relay_payload.target_function,
                         sizeof(relay_payload.target_function), "%s", owner);

                BENCH2_WORKER_TRACE("[bench2-worker] relay req=%" PRIu64 " from=%s to=%s cipher=0x%04x seq=%" PRIu64 "\n",
                                    req_no,
                                    function_name,
                                    owner,
                                    (unsigned int)relay_payload.serial.cipher_suite,
                                    relay_payload.serial.read_seq_num);

                /* detach fd from wolfSSL before relay */
                wolfSSL_set_fd(ssl, -1);
                int fd_to_relay = client_fd;
                client_fd = -1;

                if (relay_to_gateway(fd_to_relay, &relay_payload, relay_socket) != 0) {
                    fail_stage = "relay_to_gateway";
                }
                /* our turn with this session is over */
                goto done;
            }

            /*
             * Sub-case A: we ARE the owner.
             * Peek confirmed ownership; stamp top_container_before_read
             * immediately before the real consuming wolfSSL_read.
             */
            uint64_t top_cbr = bench2_rdtsc(); /* top_container_before_read */
            int req_close = 0;
            if (read_full_request(ssl, &top2, &req_close, NULL,
                                  NULL, 0) != 0) {
                fail_stage = "next_read_full_request";
                goto done;
            }

            if (send_response(ssl, function_name, req_no,
                              top1, top2, top_cbr, cntfrq_stamp, req_close) != 0) {
                fail_stage = "next_send_response";
                goto done;
            }
            if (req_close) break;
        }

        /* After each owned+served request, refresh serial for future peek */
        if (export_live_serial(ssl, &payload->serial) != 0) {
            fail_stage = "export_live_serial";
            goto done;
        }
    }

done:
    if (fail_stage) {
        fprintf(stderr, "[bench2-worker] %s closing session req=%" PRIu64 " stage=%s\n",
                function_name, req_no, fail_stage);
    }
    if (ssl) {
        wolfSSL_set_quiet_shutdown(ssl, 1);
        wolfSSL_free(ssl);
    }
    if (client_fd >= 0) close(client_fd);
}

/* ─── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    const char *fn_name      = getenv("BENCH2_FUNCTION_NAME");
    const char *socket_dir   = getenv("BENCH2_SOCKET_DIR");
    const char *relay_socket = getenv("BENCH2_RELAY_SOCKET");
    const char *cert_file    = getenv("BENCH2_CERT");
    const char *key_file     = getenv("BENCH2_KEY");

    if (!fn_name      || !fn_name[0])      fn_name      = "bench2-fn-a";
    if (!socket_dir   || !socket_dir[0])   socket_dir   = "/run/bench2";
    if (!relay_socket || !relay_socket[0]) relay_socket = "/run/bench2/relay.sock";
    if (!cert_file    || !cert_file[0])    cert_file    = "/certs/server.crt";
    if (!key_file     || !key_file[0])     key_file     = "/certs/server.key";

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, fn_name);

    signal(SIGPIPE, SIG_IGN);

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "[bench2-worker] warning: mkdir %s: %s\n",
                socket_dir, strerror(errno));
    else
        chmod(socket_dir, 0777);

    wolfSSL_Init();
    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) { fprintf(stderr, "[bench2-worker] wolfSSL_CTX_new failed\n"); return 1; }

    (void)wolfSSL_CTX_use_certificate_file(wctx, cert_file, SSL_FILETYPE_PEM);
    (void)wolfSSL_CTX_use_PrivateKey_file(wctx, key_file,   SSL_FILETYPE_PEM);

    int listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[bench2-worker] unix_server_socket failed: %s\n", strerror(errno));
        wolfSSL_CTX_free(wctx);
        return 1;
    }
    chmod(sock_path, 0777);

    fprintf(stderr, "[bench2-worker] %s listening on %s, relay=%s\n",
            fn_name, sock_path, relay_socket);

    for (;;) {
        bench2_keepalive_payload_t payload;
        int client_fd = -1;

        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) continue;

        memset(&payload, 0, sizeof(payload));
        if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
            close(conn_fd); continue;
        }
        close(conn_fd);

        if (client_fd < 0) continue;
        if (payload.magic   != BENCH2_KA_MAGIC ||
            payload.version != BENCH2_KA_VERSION) {
            fprintf(stderr, "[bench2-worker] bad magic/version 0x%08x/%u\n",
                    payload.magic, payload.version);
            close(client_fd);
            continue;
        }

        process_session(wctx, client_fd, &payload, fn_name, relay_socket);
    }
}
