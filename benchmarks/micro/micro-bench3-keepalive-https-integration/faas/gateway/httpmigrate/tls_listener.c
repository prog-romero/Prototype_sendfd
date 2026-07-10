/*
 * tls_listener.c — wolfSSL gateway bridge: non-blocking, epoll-driven.
 *
 * KEY DESIGN (mirrors bench2gw.c):
 *   - Every conn is set non-blocking immediately after accept().
 *   - TCP_NODELAY is set on every accepted connection to suppress Nagle
 *     buffering — critical for request-response latency.
 *   - wolfSSL_accept() is driven incrementally: one step per epoll event.
 *   - tlsgw_peek_and_export_nb() never blocks: returns 0 (EAGAIN) if the TLS
 *     record is not yet fully arrived in the kernel socket buffer.
 *   - No per-connection threads or goroutines are created.
 *   - The Go side runs ONE shared epoll loop that handles ALL state transitions.
 *
 * CIPHER PREFERENCE:
 *   TLS_AES_128_GCM_SHA256 is listed first because the ARM Cortex-A72 (Pi 4)
 *   has hardware AES acceleration (ARMv8 Cryptographic Extensions). AES-128
 *   has fewer rounds than AES-256, making it faster while still secure for
 *   this workload. Clients negotiate from the server's preference list.
 *
 * MODE SPLIT:
 *   wolfssl_accept_start()       — prototype path: installs tlspeek keylog
 *                                  callback so the TLS session can be exported.
 *   wolfssl_accept_start_plain() — vanilla path: no keylog callback, no
 *                                  libtlspeek involvement at any point.
 */

/* _GNU_SOURCE must come FIRST to expose accept4(), SOCK_NONBLOCK, SOCK_CLOEXEC */
#define _GNU_SOURCE

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include "tls_listener.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <tlspeek/tlspeek.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>     /* TCP_NODELAY */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── TLSGW_SERIAL_SIZE ───────────────────────────────────────────────────── */
const int TLSGW_SERIAL_SIZE = (int)sizeof(tlspeek_serial_t);

/* ── Internal structs ────────────────────────────────────────────────────── */

struct wolfssl_gtw_ctx {
    WOLFSSL_CTX *wctx;  
};

struct wolfssl_gtw_conn {
    int                       tcp_fd;
    WOLFSSL                  *ssl;
    tlspeek_ctx_t             peek_ctx;   /* zeroed in vanilla path */
    wolfssl_gtw_conn_state_t  state;
    int                       want_events; /* 1=EPOLLIN, 2=EPOLLOUT */

    /* wolfSSL may buffer application data during wolfSSL_accept() when the
     * client pipelines the first HTTP request with the TLS Finished message.
     * In that case FIONREAD on the raw TCP socket is 0, tls_record_ready()
     * returns 0, and EPOLLET never fires again — connection stalls forever.
     * We drain wolfSSL's internal buffer into pending_buf here for replay:
     *   /function/ path      → stored in serial->http_request for the worker.
     *   non-function path    → served by wolfssl_gtw_conn_read() (ChanListener).
     * In vanilla mode pending_buf is also used for the same ChanListener replay.
     * Size matches TLSPEEK_MAX_REQUEST_SZ so pipelined bodies are never truncated
     * before being copied into serial->http_request. */
    char                      pending_buf[8192];
    int                       pending_len;
    int                       pending_off;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Disable Nagle algorithm on a TCP socket.
 * Without TCP_NODELAY, the kernel may buffer a small TLS write for up to
 * ~200 ms waiting to coalesce with more data — catastrophic for latency in
 * request-response protocols.  Set this immediately after accept4(). */
static void set_tcp_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* Check if at least one complete TLS record is in the kernel buffer.
 * Uses FIONREAD + MSG_PEEK of the 5-byte TLS header.
 * Returns: 1=ready, 0=not yet, -1=error/eof */
static int tls_record_ready(int fd)
{
    unsigned char hdr[5];
    ssize_t n;
    do {
        n = recv(fd, hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n == 0) return -1;        /* peer closed */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if ((size_t)n < sizeof(hdr)) return 0;  /* partial header */

    uint16_t record_len = (uint16_t)((hdr[3] << 8) | hdr[4]);
    /* Sanity: TLS record payload ≤ 2^14 + 256 bytes per RFC 8446 */
    if (record_len > 16384 + 256) return -1;

    int avail = 0;
    if (ioctl(fd, FIONREAD, &avail) != 0) return 0;
    return ((size_t)avail >= sizeof(hdr) + (size_t)record_len) ? 1 : 0;
}

/* Parse "/function/<name>" from the first line of a plaintext HTTP request.
 * Returns 1 and fills out/out_sz on success; 0 if not a /function/ path. */
static int parse_fn_name(const unsigned char *buf, size_t len,
                          char *out, size_t out_sz)
{
    if (!buf || !len || !out || !out_sz) return 0;

    size_t eol = 0;
    while (eol < len && buf[eol] != '\n') eol++;

    const unsigned char *sp1 = memchr(buf, ' ', eol);
    if (!sp1) return 0;

    const unsigned char *path = sp1 + 1;
    static const char prefix[] = "/function/";
    size_t plen = eol - (size_t)(path - buf);
    const unsigned char *sp2 = memchr(path, ' ', plen);
    if (!sp2) return 0;

    size_t rlen = (size_t)(sp2 - path);
    if (rlen <= sizeof(prefix) - 1 ||
        memcmp(path, prefix, sizeof(prefix) - 1) != 0)
        return 0;

    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++) {
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            nlen = i;
            break;
        }
    }
    if (!nlen || nlen + 1 > out_sz) return 0;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return 1;
}

/* Map a wolfSSL cipher name to the tlspeek cipher enum.
 * wolfSSL returns "TLS13-AES128-GCM-SHA256", "TLS13-AES256-GCM-SHA384",
 * "TLS13-CHACHA20-POLY1305-SHA256" — note "AES128" without a hyphen. */
static void set_serial_cipher(tlspeek_serial_t *s, const char *name)
{
    if (!s) return;
    if (!name)                                             { s->cipher_suite = TLSPEEK_AES_256_GCM; return; }
    if (strstr(name, "CHACHA20"))                            s->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(name, "AES128") || strstr(name, "AES-128")) s->cipher_suite = TLSPEEK_AES_128_GCM;
    else                                                     s->cipher_suite = TLSPEEK_AES_256_GCM;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

wolfssl_gtw_ctx_t *wolfssl_gtw_ctx_new(const char *cert_file,
                                        const char *key_file)
{
    if (!cert_file || !key_file) return NULL;

    wolfSSL_Init();

    wolfssl_gtw_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx->wctx) { free(ctx); return NULL; }

    /* Force TLS 1.3 only — older versions are insecure and not needed here. */
    wolfSSL_CTX_SetMinVersion(ctx->wctx, WOLFSSL_TLSV1_3);
    wolfSSL_CTX_set_max_proto_version(ctx->wctx, WOLFSSL_TLSV1_3);

    /* Prefer AES-128-GCM first: on ARM Cortex-A72 (Pi 4) the ARMv8 Crypto
     * Extensions accelerate AES in hardware.  AES-128 has fewer key expansion
     * rounds than AES-256, so it is ~20 % faster for the same hardware unit.
     * AES-256 is still offered as a fallback; ChaCha20 last (no hardware unit
     * on Cortex-A72, so it runs in software and is slower). */
    wolfSSL_CTX_set_cipher_list(ctx->wctx,
        "TLS13-AES128-GCM-SHA256:"
        "TLS13-AES256-GCM-SHA384:"
        "TLS13-CHACHA20-POLY1305-SHA256");

    if (wolfSSL_CTX_use_certificate_file(ctx->wctx, cert_file,
                                          SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(ctx->wctx, key_file,
                                         SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(ctx->wctx);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void wolfssl_gtw_ctx_free(wolfssl_gtw_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->wctx) wolfSSL_CTX_free(ctx->wctx);
    free(ctx);
    wolfSSL_Cleanup();
}

/* ── Accept + start non-blocking handshake (prototype path) ─────────────── */

wolfssl_gtw_conn_t *wolfssl_accept_start(wolfssl_gtw_ctx_t *ctx, int listen_fd)
{
    if (!ctx || !ctx->wctx || listen_fd < 0) { errno = EINVAL; return NULL; }

    /* accept4 sets O_NONBLOCK atomically — avoids a separate fcntl() call. */
    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) return NULL;  /* errno = EAGAIN when no conn is pending */

    /* Disable Nagle buffering immediately — critical for request/response latency. */
    set_tcp_nodelay(client_fd);

    wolfssl_gtw_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { close(client_fd); errno = ENOMEM; return NULL; }

    conn->tcp_fd      = client_fd;
    conn->state       = WOLFSSL_GTW_CONN_HANDSHAKE;
    conn->want_events = 1; /* Start waiting for EPOLLIN */

    conn->ssl = wolfSSL_new(ctx->wctx);
    if (!conn->ssl) { free(conn); close(client_fd); errno = ENOMEM; return NULL; }

    wolfSSL_set_fd(conn->ssl, client_fd);
    wolfSSL_set_using_nonblock(conn->ssl, 1);

    /* Install tlspeek keylog callback so session keys are captured during
     * the handshake and available for tls_read_peek() / wolfSSL_tls_export(). */
    memset(&conn->peek_ctx, 0, sizeof(conn->peek_ctx));
    conn->peek_ctx.tcp_fd     = client_fd;
    conn->peek_ctx.ssl        = conn->ssl;
    conn->peek_ctx.keys_ready = 1;

    wolfSSL_set_ex_data(conn->ssl, 0, &conn->peek_ctx);
    wolfSSL_set_tls13_secret_cb(conn->ssl, tlspeek_keylog_cb, &conn->peek_ctx);

    return conn;
}

/* ── Accept + start non-blocking handshake (vanilla path) ───────────────── */

/*
 * wolfssl_accept_start_plain() — identical to wolfssl_accept_start() EXCEPT
 * that no tlspeek keylog callback is installed.  This means:
 *   - peek_ctx is left zeroed (tlspeek_free() is a no-op on a zeroed ctx).
 *   - tls_read_peek() is never called on this connection.
 *   - libtlspeek is completely uninvolved in the connection lifetime.
 *
 * Used by RunVanillaHTTPS (HTTPMIGRATE_ENABLE=0): after the handshake
 * completes, the conn is pushed directly to ChanListener and all subsequent
 * reads/writes go through wolfSSL_read/wolfSSL_write only.
 */
wolfssl_gtw_conn_t *wolfssl_accept_start_plain(wolfssl_gtw_ctx_t *ctx, int listen_fd)
{
    if (!ctx || !ctx->wctx || listen_fd < 0) { errno = EINVAL; return NULL; }

    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) return NULL;

    /* Disable Nagle buffering — same reason as in wolfssl_accept_start(). */
    set_tcp_nodelay(client_fd);

    wolfssl_gtw_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { close(client_fd); errno = ENOMEM; return NULL; }

    conn->tcp_fd      = client_fd;
    conn->state       = WOLFSSL_GTW_CONN_HANDSHAKE;
    conn->want_events = 1;

    conn->ssl = wolfSSL_new(ctx->wctx);
    if (!conn->ssl) { free(conn); close(client_fd); errno = ENOMEM; return NULL; }

    wolfSSL_set_fd(conn->ssl, client_fd);
    wolfSSL_set_using_nonblock(conn->ssl, 1);
    /* peek_ctx left zeroed — no keylog callback, no libtlspeek. */

    return conn;
}

int wolfssl_conn_fd(const wolfssl_gtw_conn_t *conn)
{
    return conn ? conn->tcp_fd : -1;
}

int wolfssl_conn_want_events(const wolfssl_gtw_conn_t *conn)
{
    return conn ? conn->want_events : 1;
}

wolfssl_gtw_conn_state_t wolfssl_conn_state(const wolfssl_gtw_conn_t *conn)
{
    return conn ? conn->state : WOLFSSL_GTW_CONN_DONE;
}

/* ── Drive one step of the non-blocking TLS handshake ───────────────────── */

int wolfssl_handshake_step(wolfssl_gtw_conn_t *conn)
{
    if (!conn || !conn->ssl) return -1;

    int rc = wolfSSL_accept(conn->ssl);
    if (rc == SSL_SUCCESS) {
        /* Handshake complete — record the negotiated cipher in peek_ctx so
         * set_serial_cipher() can fill it into the TLS export blob later. */
        const char *cname = wolfSSL_get_cipher_name(conn->ssl);
        if (cname) {
            /* wolfSSL cipher name format: "TLS13-AES128-GCM-SHA256"
             *                              "TLS13-AES256-GCM-SHA384"
             *                              "TLS13-CHACHA20-POLY1305-SHA256"
             * Note: "AES128" (no hyphen before 128), so check for "AES128", NOT "AES-128". */
            if (strstr(cname, "CHACHA20"))
                conn->peek_ctx.cipher_suite = TLSPEEK_CHACHA20_POLY;
            else if (strstr(cname, "AES128") || strstr(cname, "AES-128"))
                conn->peek_ctx.cipher_suite = TLSPEEK_AES_128_GCM;
            else
                conn->peek_ctx.cipher_suite = TLSPEEK_AES_256_GCM;
        }
        conn->state       = WOLFSSL_GTW_CONN_PEEK;
        conn->want_events = 1; /* Wait for first request data (EPOLLIN) */
        return 1;
    }

    int err = wolfSSL_get_error(conn->ssl, rc);
    if (err == WOLFSSL_ERROR_WANT_READ) {
        conn->want_events = 1;  /* Need more data from client */
        return 0;
    }
    if (err == WOLFSSL_ERROR_WANT_WRITE) {
        conn->want_events = 2;  /* Need to flush our handshake messages */
        return 0;
    }

    /* Any other error is fatal (bad cert, protocol mismatch, peer reset…). */
    return -1;
}

/* ── Non-blocking peek + export (prototype path only) ───────────────────── */

int tlsgw_peek_and_export_nb(
    wolfssl_gtw_conn_t *conn,
    char               *fn_name_out,
    size_t              fn_name_sz,
    uint64_t           *top1_ns_out,
    void               *serial_buf,
    int                *serial_sz_out,
    int                 skip_top1)
{
    if (!conn || !fn_name_out || fn_name_sz == 0) return -2;

    fn_name_out[0] = '\0';
    if (top1_ns_out)   *top1_ns_out   = 0;
    if (serial_sz_out) *serial_sz_out = 0;

    /* ── Step 1: obtain plaintext of the incoming HTTP request ───────────────
     *
     * Three sources, tried in priority order:
     *
     * A) conn->pending_buf — data already drained from wolfSSL's internal
     *    buffer on a previous call that returned -1 (non-function path re-entry).
     *
     * B) wolfSSL_pending() > 0 — wolfSSL consumed the client's pipelined HTTP
     *    request from the TCP socket during wolfSSL_accept().  The kernel buffer
     *    now shows FIONREAD=0 so tls_record_ready() would return 0, EPOLLET
     *    would never fire, and the connection would stall forever.
     *    Fix: drain wolfSSL's buffer via wolfSSL_read(), store in pending_buf
     *    for the non-function (ChanListener) path to replay later.
     *
     * C) Normal path — data arrived after handshake in the kernel TCP buffer.
     *    Use tls_record_ready() + tls_read_peek() (stateless MSG_PEEK decrypt).
     */
    const uint8_t *plaintext;
    int            plaintext_len;
    /* 256 bytes is enough to parse the HTTP request line (e.g.
     * "POST /function/timing-fn-ka-worker HTTP/1.1\r\n" ≈ 50 bytes).
     * tls_read_peek uses MSG_PEEK — data stays in the socket for the worker. */
    uint8_t        peek_buf[256];
    int            from_pending = 0;   /* 1 = data from wolfSSL internal buffer */

    if (conn->pending_len > conn->pending_off) {
        /* Path A: leftover from a previous non-function call */
        plaintext     = (const uint8_t *)conn->pending_buf + conn->pending_off;
        plaintext_len = conn->pending_len - conn->pending_off;
        from_pending  = 1;

    } else if (wolfSSL_pending(conn->ssl) > 0) {
        /* Path B: wolfSSL pipelining — drain internal buffer.
         * wolfSSL_read() here does NOT consume TCP bytes; wolfSSL already
         * pulled them during accept().  Reserve one byte for NUL terminator. */
        int n = wolfSSL_read(conn->ssl, conn->pending_buf,
                             (int)sizeof(conn->pending_buf) - 1);
        if (n <= 0) {
            int e = wolfSSL_get_error(conn->ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) {
                conn->want_events = (e == WOLFSSL_ERROR_WANT_WRITE) ? 2 : 1;
                return 0;
            }
            fprintf(stderr, "[tlsgw] Path B: wolfSSL_read failed (err=%d) fd=%d\n",
                    e, conn->tcp_fd);
            wolfssl_conn_free(conn);
            return -2;
        }
        conn->pending_buf[n] = '\0';
        conn->pending_len    = n;
        conn->pending_off    = 0;
        plaintext     = (const uint8_t *)conn->pending_buf;
        plaintext_len = n;
        from_pending  = 1;

    } else {
        /* Path C: normal — check kernel TCP socket for a complete TLS record */
        int ready = tls_record_ready(conn->tcp_fd);
        if (ready == 0) {
            conn->want_events = 1;
            return 0;   /* EAGAIN — re-arm EPOLLIN and wait */
        }
        if (ready < 0) {
            fprintf(stderr, "[tlsgw] Path C: tls_record_ready error fd=%d\n", conn->tcp_fd);
            wolfssl_conn_free(conn);
            return -2;
        }

        /* Stateless TLS decrypt via libtlspeek (MSG_PEEK — does not consume) */
        int peeked = tls_read_peek(&conn->peek_ctx, peek_buf,
                                   (int)sizeof(peek_buf) - 1);
        if (peeked == 0) {
            conn->want_events = 1;
            return 0;
        }
        if (peeked < 0) {
            fprintf(stderr, "[tlsgw] Path C: tls_read_peek failed fd=%d\n", conn->tcp_fd);
            wolfssl_conn_free(conn);
            return -2;
        }
        peek_buf[peeked] = '\0';
        plaintext     = peek_buf;
        plaintext_len = peeked;
    }

    /* ── Step 2: stamp top1 timestamp (earliest possible point) ─────────── */
    if (!skip_top1 && top1_ns_out)
        *top1_ns_out = now_ns();

    /* ── Step 3: parse /function/<name> from the HTTP request line ───────── */
    if (!parse_fn_name(plaintext, (size_t)plaintext_len, fn_name_out, fn_name_sz)) {
        /* Not a /function/ path — leave wolfSSL active for ChanListener.
         * pending_buf already holds the data (paths A/B) for wolfssl_gtw_conn_read(). */
        return -1;
    }

    /* ── Step 4: export full TLS session state for sendfd ────────────────── */
    tlspeek_serial_t *serial = (tlspeek_serial_t *)serial_buf;
    if (!serial) {
        wolfssl_conn_free(conn);
        return -2;
    }

    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    set_serial_cipher(serial, wolfSSL_get_cipher_name(conn->ssl));

    /* wolfSSL_tls_export() serialises the full TLS 1.3 session state
     * (keys, sequence numbers, cipher state) into serial->tls_blob.
     * The worker will import this blob and resume the session on the raw fd. */
    unsigned int blob_sz = TLSPEEK_MAX_EXPORT_SZ;
    int rc = wolfSSL_tls_export(conn->ssl, serial->tls_blob, &blob_sz);
    if (rc <= 0) {
        int werr = wolfSSL_get_error(conn->ssl, rc);
        fprintf(stderr, "[tlsgw] wolfSSL_tls_export FAILED rc=%d werr=%d fd=%d\n",
                rc, werr, conn->tcp_fd);
        wolfssl_conn_free(conn);
        return -2;
    }
    serial->blob_sz = blob_sz;

    /* Copy the session keys captured by tlspeek_keylog_cb during handshake.
     * These are needed for tls_read_peek() on the worker side when a
     * subsequent request arrives before the worker calls wolfSSL_read(). */
    memcpy(serial->client_write_key,
           conn->peek_ctx.client_write_key,
           sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv,
           conn->peek_ctx.client_write_iv,
           sizeof(serial->client_write_iv));
    serial->read_seq_num = conn->peek_ctx.read_seq_num;

    /* Store pre-read request data so the worker can replay it.
     * Without this, the worker would call wolfSSL_read() and block waiting for
     * data that wolfSSL already consumed from the kernel buffer during our peek. */
    if (from_pending && plaintext_len > 0) {
        int copy_len = plaintext_len;
        if (copy_len > (int)(sizeof(serial->http_request) - 1))
            copy_len = (int)(sizeof(serial->http_request) - 1);
        memcpy(serial->http_request, plaintext, (size_t)copy_len);
        serial->request_len = copy_len;
    }

    if (serial_sz_out) *serial_sz_out = (int)sizeof(tlspeek_serial_t);

    /* Detach wolfSSL from tcp_fd — returns the raw fd for sendfd.
     * quiet_shutdown prevents sending a TLS close_notify (the worker owns
     * the session now and will send it when appropriate). */
    int raw_fd = conn->tcp_fd;
    wolfSSL_set_quiet_shutdown(conn->ssl, 1);
    wolfSSL_set_fd(conn->ssl, -1);
    wolfSSL_free(conn->ssl);
    conn->ssl    = NULL;
    conn->tcp_fd = -1;
    tlspeek_free(&conn->peek_ctx);
    conn->state = WOLFSSL_GTW_CONN_DONE;
    free(conn);

    return raw_fd;  /* caller sends this raw TCP fd to the worker via SCM_RIGHTS */
}

/* ── Conn management ─────────────────────────────────────────────────────── */

void wolfssl_conn_free(wolfssl_gtw_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
    }
    if (conn->tcp_fd >= 0) close(conn->tcp_fd);
    tlspeek_free(&conn->peek_ctx);
    free(conn);
}

/* Detach and return the raw TCP fd from a conn without freeing wolfSSL.
 * Used before pushing to ChanListener: WolfSSLGtwConn.Close() will free ssl. */
int wolfssl_conn_take_raw_fd(wolfssl_gtw_conn_t *conn)
{
    if (!conn) return -1;
    int fd = conn->tcp_fd;
    conn->tcp_fd = -1;
    return fd;
}

/* ── ChanListener path: wolfSSL read/write (both vanilla and proto) ──────── */

/*
 * wolfssl_gtw_conn_read() — Read plaintext from the wolfSSL session.
 *
 * First drains pending_buf (data that was pre-read from wolfSSL's internal
 * buffer during the handshake pipeline detection in tlsgw_peek_and_export_nb
 * or during wolfSSL pipelining in vanilla mode).  Falls through to a direct
 * wolfSSL_read() when the pending buffer is exhausted.
 */
int wolfssl_gtw_conn_read(wolfssl_gtw_conn_t *conn, void *buf, int len)
{
    if (!conn || !buf || len <= 0) return -1;

    /* Replay any data that was pre-buffered during the handshake. */
    if (conn->pending_len > conn->pending_off) {
        int avail = conn->pending_len - conn->pending_off;
        int copy  = avail < len ? avail : len;
        memcpy(buf, conn->pending_buf + conn->pending_off, (size_t)copy);
        conn->pending_off += copy;
        if (conn->pending_off >= conn->pending_len) {
            conn->pending_len = 0;
            conn->pending_off = 0;
        }
        return copy;
    }

    if (!conn->ssl) return -1;
    return wolfSSL_read(conn->ssl, buf, len);
}

/*
 * wolfssl_gtw_conn_write() — Write plaintext through the wolfSSL session.
 *
 * Returns the wolfSSL_write() return value directly.  The Go caller checks
 * the error code and handles WANT_READ / WANT_WRITE by waiting on epoll and
 * retrying with the same buffer — wolfSSL requires the EXACT same arguments
 * on retry.
 *
 * NOTE: Do NOT call wolfSSL_read() here when wolfSSL_write returns WANT_READ.
 * WANT_READ during write means wolfSSL must process a pending TLS 1.3
 * handshake message (e.g. KeyUpdate) from the peer before it can send.
 * wolfSSL handles this internally on the next wolfSSL_write() call — no
 * explicit drain is needed, and draining here would silently discard any
 * application data the peer may have sent concurrently.
 */
int wolfssl_gtw_conn_write(wolfssl_gtw_conn_t *conn, const void *buf, int len)
{
    if (!conn || !conn->ssl || !buf || len <= 0) return -1;
    return wolfSSL_write(conn->ssl, buf, len);
}

int wolfssl_gtw_conn_get_error(wolfssl_gtw_conn_t *conn, int ret)
{
    if (!conn || !conn->ssl) return -1;
    return wolfSSL_get_error(conn->ssl, ret);
}

int wolfssl_gtw_conn_fd(const wolfssl_gtw_conn_t *conn)
{
    return conn ? conn->tcp_fd : -1;
}

/* Returns the number of bytes buffered inside wolfSSL that have been
 * decrypted but not yet consumed by the application.  The Go side calls
 * this via Pending() to check whether to drain before waiting on epoll. */
int wolfssl_gtw_conn_pending(const wolfssl_gtw_conn_t *conn)
{
    if (!conn || !conn->ssl) return 0;
    return wolfSSL_pending(conn->ssl);
}

/* Close the wolfSSL session and the underlying TCP fd.
 * quiet_shutdown avoids sending a TLS close_notify — the peer already
 * triggered close (FIN or RST) by the time we reach here in most cases. */
void wolfssl_gtw_conn_close(wolfssl_gtw_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_set_quiet_shutdown(conn->ssl, 1);
        wolfSSL_free(conn->ssl);
    }
    if (conn->tcp_fd >= 0) close(conn->tcp_fd);
    tlspeek_free(&conn->peek_ctx);
    free(conn);
}
