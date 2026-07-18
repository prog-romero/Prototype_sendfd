/*
 * wd_bridge.c — full-proxy connection driver (CGO bridge).
 *
 * Blocking, one-connection-per-call port of the C function workers:
 *   - HTTP  : timing_fn_ka_worker.c
 *   - HTTPS : timing_fn_ka_worker_https.c
 *
 * The plumbing (owner peek, relay, keep-alive, request framing, TLS restore /
 * peek / read / write / re-export) is reproduced here. The business logic is
 * delegated to Go via goInvokeHandler(). See wd_bridge.h for the contract.
 *
 * Only compiled when CGO is enabled with the wolfSSL toolchain available
 * (the "wolfssl"-tagged build); the CGO-disabled build ignores this file.
 */
#define _GNU_SOURCE

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <tlspeek/tlspeek.h>

#include "wd_bridge.h"

/* ── Wire constants (must match the gateway payload structs) ─────────────── */

#define HTTPMIGRATE_MAGIC_HTTP   0x484D4B41U   /* 'HMKA' */
#define HTTPMIGRATE_HTTPS_MAGIC  0x484D4B53U   /* 'HMKS' */
#define HTTPMIGRATE_VERSION_HTTP 2U
#define HTTPMIGRATE_TARGET_LEN   128
#define CNTFRQ                   1000000000ULL

/* Base header: first 160 bytes — matches KAPayload in the gateway. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t  top1_set;
    uint8_t  _pad[7];
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} wd_base_t;

/* Full HTTPS payload = base + serialised TLS state. */
typedef struct {
    wd_base_t        base;
    tlspeek_serial_t serial;
} wd_https_payload_t;

static WOLFSSL_CTX *s_wctx = NULL;

/* ── Small helpers ──────────────────────────────────────────────────────── */

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

static void clear_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static ssize_t find_subseq(const unsigned char *b, size_t l, const char *n)
{
    size_t nl = strlen(n);
    if (!nl || l < nl) return -1;
    for (size_t i = 0; i + nl <= l; i++)
        if (memcmp(b + i, n, nl) == 0) return (ssize_t)i;
    return -1;
}

static long long parse_cl(const char *h, size_t hl)
{
    const char *p = h, *e = h + hl;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(e - p);
        const char *nd = "content-length:";
        size_t nlen = 15;
        if (ll >= nlen) {
            int m = 1;
            for (size_t k = 0; k < nlen && m; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                m = (c == nd[k]);
            }
            if (m) {
                const char *v = p + nlen;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                char *ep = NULL;
                long long val = strtoll(v, &ep, 10);
                if (ep && ep > v) return val;
            }
        }
        p = nl ? nl + 1 : e;
    }
    return -1;
}

static int has_close(const char *h, size_t hl)
{
    return find_subseq((const unsigned char *)h, hl, "Connection: close") >= 0 ||
           find_subseq((const unsigned char *)h, hl, "connection: close") >= 0;
}

/* Extract the function name from "<METHOD> /function/<name>[/...] HTTP/1.1". */
static int parse_owner(const unsigned char *buf, size_t len, char *out, size_t outsz)
{
    if (!buf || !len || !out || !outsz) return 0;
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
        memcmp(path, prefix, sizeof(prefix) - 1) != 0) return 0;
    const unsigned char *name = path + sizeof(prefix) - 1;
    size_t nlen = rlen - (sizeof(prefix) - 1);
    for (size_t i = 0; i < nlen; i++)
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') { nlen = i; break; }
    if (!nlen || nlen + 1 > outsz) return 0;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return 1;
}

static int ensure_cap(unsigned char **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) return 0;
    size_t nc = *cap ? *cap : 16384;
    while (nc < needed) nc *= 2;
    unsigned char *nb = realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

/* ── Relay: forward a wrong-owner connection to the provider socket ──────── */

static int connect_seqpacket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int sendfd_payload(int unix_sock, int fd, const void *payload, size_t plen)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct iovec iov = { .iov_base = (void *)payload, .iov_len = plen };
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf),
    };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return (sendmsg(unix_sock, &msg, 0) < 0) ? -1 : 0;
}

static void relay_http(int fd, const char *owner, const char *relay_sock)
{
    wd_base_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.magic      = HTTPMIGRATE_MAGIC_HTTP;
    rp.version    = HTTPMIGRATE_VERSION_HTTP;
    rp.top1_rdtsc = now_ns();
    rp.cntfrq     = CNTFRQ;
    rp.top1_set   = 1;
    snprintf(rp.target_function, sizeof(rp.target_function), "%s", owner);

    int rfd = connect_seqpacket(relay_sock);
    if (rfd >= 0) {
        (void)sendfd_payload(rfd, fd, &rp, sizeof(rp));
        close(rfd);
    }
    close(fd);
}

/* ── HTTPS: TLS state export (matches the worker's export_serial) ────────── */

static void set_cipher(tlspeek_serial_t *s, const char *n)
{
    if (!s) return;
    if (!n) { s->cipher_suite = TLSPEEK_AES_256_GCM; return; }
    if (strstr(n, "CHACHA20")) s->cipher_suite = TLSPEEK_CHACHA20_POLY;
    else if (strstr(n, "AES128") || strstr(n, "AES-128")) s->cipher_suite = TLSPEEK_AES_128_GCM;
    else s->cipher_suite = TLSPEEK_AES_256_GCM;
}

static int export_serial(WOLFSSL *ssl, tlspeek_serial_t *serial)
{
    if (!ssl || !serial) return -1;
    memset(serial, 0, sizeof(*serial));
    serial->magic = TLSPEEK_MAGIC;
    set_cipher(serial, wolfSSL_get_cipher_name(ssl));
    unsigned int sz = TLSPEEK_MAX_EXPORT_SZ;
    int rc = wolfSSL_tls_export(ssl, serial->tls_blob, &sz);
    serial->blob_sz = (rc > 0) ? sz : 0;
    const unsigned char *k = wolfSSL_GetClientWriteKey(ssl);
    const unsigned char *iv = wolfSSL_GetClientWriteIV(ssl);
    int ksz = wolfSSL_GetKeySize(ssl), ivsz = wolfSSL_GetIVSize(ssl);
    if (!k || !iv || ksz <= 0 || ivsz <= 0) return -1;
    memcpy(serial->client_write_key, k,
           (size_t)ksz < sizeof(serial->client_write_key) ? (size_t)ksz : sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv, iv,
           (size_t)ivsz < sizeof(serial->client_write_iv) ? (size_t)ivsz : sizeof(serial->client_write_iv));
    word64 seq = 0;
    if (wolfSSL_GetPeerSequenceNumber(ssl, &seq) < 0) return -1;
    serial->read_seq_num = (uint64_t)seq;
    return 0;
}

static void relay_https(int fd, WOLFSSL *ssl, wd_https_payload_t *pl,
                        const char *owner, const char *relay_sock)
{
    memset(pl->base.target_function, 0, sizeof(pl->base.target_function));
    strncpy(pl->base.target_function, owner, sizeof(pl->base.target_function) - 1);
    pl->base.magic    = HTTPMIGRATE_HTTPS_MAGIC;
    pl->base.cntfrq   = CNTFRQ;
    pl->base.top1_set = 1;
    export_serial(ssl, &pl->serial);
    wolfSSL_set_fd(ssl, -1);
    wolfSSL_free(ssl);

    int rfd = connect_seqpacket(relay_sock);
    if (rfd >= 0) {
        (void)sendfd_payload(rfd, fd, pl, sizeof(*pl));
        close(rfd);
    }
    close(fd);
}

/* ── HTTP connection driver ─────────────────────────────────────────────── */

static void serve_http_conn(int fd, int pipe_fd, const char *own,
                            const char *relay_sock, uintptr_t h)
{
    clear_nonblocking(fd);
    unsigned char *buf = NULL;
    size_t cap = 0;

    for (;;) {
        /* 1. Peek the first line to decide handle-vs-relay (no consume). */
        unsigned char peek[1024];
        ssize_t pn = recv(fd, peek, sizeof(peek) - 1, MSG_PEEK);
        if (pn <= 0) break;
        peek[pn] = '\0';
        if (memchr(peek, '\n', (size_t)pn)) {
            char owner[HTTPMIGRATE_TARGET_LEN];
            if (parse_owner(peek, (size_t)pn, owner, sizeof(owner)) &&
                strcmp(owner, own) != 0) {
                relay_http(fd, owner, relay_sock);
                fd = -1;
                goto done;
            }
        }

        /* 2. Peek until the header terminator is visible (no consume). */
        size_t hdr_sz = 0;
        int found = 0;
        for (int tries = 0; tries < 4000; tries++) {
            unsigned char tmp[16384];
            ssize_t tn = recv(fd, tmp, sizeof(tmp), MSG_PEEK);
            if (tn <= 0) goto done;
            size_t sep = 4;
            ssize_t e = find_subseq(tmp, (size_t)tn, "\r\n\r\n");
            if (e < 0) { e = find_subseq(tmp, (size_t)tn, "\n\n"); sep = 2; }
            if (e >= 0) { hdr_sz = (size_t)e + sep; found = 1; break; }
            if ((size_t)tn >= sizeof(tmp)) goto done; /* header too large */
            usleep(500);
        }
        if (!found) goto done;

        /* 3. Drain EXACTLY the headers (leave the next request untouched). */
        if (ensure_cap(&buf, &cap, hdr_sz + 1) != 0) goto done;
        size_t got = 0;
        while (got < hdr_sz) {
            ssize_t n = recv(fd, buf + got, hdr_sz - got, 0);
            if (n <= 0) goto done;
            got += (size_t)n;
        }
        buf[hdr_sz] = '\0';

        long long cl = parse_cl((const char *)buf, hdr_sz);
        size_t body = (cl < 0) ? 0 : (size_t)cl;
        int sc = has_close((const char *)buf, hdr_sz);

        /* 4. Drain EXACTLY the body. */
        if (ensure_cap(&buf, &cap, hdr_sz + body + 1) != 0) goto done;
        size_t bgot = 0;
        while (bgot < body) {
            ssize_t n = recv(fd, buf + hdr_sz + bgot, body - bgot, 0);
            if (n <= 0) goto done;
            bgot += (size_t)n;
        }
        size_t req_sz = hdr_sz + body;
        buf[req_sz] = '\0';

        /* 5. Business logic in Go, then write the response directly. */
        unsigned char *resp = NULL;
        int resp_len = 0;
        if (goInvokeHandler(h, buf, (int)req_sz, sc, &resp, &resp_len) != 0) goto done;
        size_t off = 0;
        while (off < (size_t)resp_len) {
            ssize_t n = send(fd, resp + off, (size_t)resp_len - off, 0);
            if (n <= 0) { free(resp); goto done; }
            off += (size_t)n;
        }
        free(resp);

        if (pipe_fd >= 0) {
            uint64_t ts = now_ns();
            (void)write(pipe_fd, &ts, sizeof(ts));
            close(pipe_fd);
            pipe_fd = -1;
        }
        if (sc) break;
    }

done:
    free(buf);
    if (fd >= 0) close(fd);
    if (pipe_fd >= 0) {
        uint64_t ts = now_ns();
        (void)write(pipe_fd, &ts, sizeof(ts));
        close(pipe_fd);
    }
}

/* ── HTTPS connection driver ────────────────────────────────────────────── */

static void serve_https_conn(int fd, int pipe_fd, wd_https_payload_t *pl,
                             const char *own, const char *relay_sock, uintptr_t h)
{
    /* All locals declared up front so the early `goto done` paths never jump
     * over an initializer. */
    WOLFSSL *ssl = NULL;
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;
    int first = 1;
    int peek_retries = 0;   /* bounds the partial-record wait so a stalled
                               connection can never busy-loop a CPU forever */

    clear_nonblocking(fd);

    ssl = wolfSSL_new(s_wctx);
    if (!ssl) goto done;
    wolfSSL_set_fd(ssl, fd);
    /* [MICROBENCH] net TLS deserialization time = session restore (tls_import). */
    uint64_t mb_de0 = now_ns();
    if (tlspeek_restore(ssl, &pl->serial) != 0) { wolfSSL_free(ssl); ssl = NULL; goto done; }
    fprintf(stderr, "[MICROBENCH] tls_deserialize_ns=%llu\n",
            (unsigned long long)(now_ns() - mb_de0));
    wolfSSL_set_fd(ssl, fd);
    /* The migrated session must not emit a NewSessionTicket (there is no session
     * resumption on this path): disabling it avoids interleaving a handshake
     * record before the response app-data. Must be set AFTER the import, which
     * restores options.noTicketTls13 from the exported state. */
    wolfSSL_no_ticket_TLSv13(ssl);

    for (;;) {
        /* [MICROBENCH] remember whether this iteration assembles the FIRST
         * (migrated) request, so we can stamp top2 once it is fully read. */
        int mb_is_first = first;
        /* Owner peek only when the buffer is empty (start of a request). */
        if (len == 0) {
            if (first && pl->serial.request_len > 0) {
                /* Replay the request the gateway consumed during the handshake. */
                size_t pre = (size_t)pl->serial.request_len;
                if (ensure_cap(&buf, &cap, pre + 1) != 0) goto done;
                memcpy(buf, pl->serial.http_request, pre);
                len = pre;
                buf[len] = '\0';
                pl->serial.request_len = 0;
            } else if (first && pl->base.top1_set) {
                /* Pre-routed connection (relayed via the provider): the owner is
                 * already this function, so skip the peek and read directly,
                 * exactly like the C worker's WS_READ_REQ initial state. */
            } else {
                tlspeek_ctx_t pctx;
                if (tlspeek_restore_peek_ctx(&pctx, fd, &pl->serial) != 0) goto done;
                uint8_t peek[4096];
                int pn = tls_read_peek(&pctx, peek, sizeof(peek) - 1);
                tlspeek_free(&pctx);
                if (pn < 0) goto done;
                if (pn == 0) {
                    /* Partial TLS record in the kernel buffer: wait briefly for
                     * the rest, but give up after ~4s so a stalled peer cannot
                     * pin a CPU. A live keep-alive idle (no bytes at all) blocks
                     * inside tls_read_peek's recv() instead, using no CPU. */
                    if (++peek_retries > 4000) goto done;
                    usleep(1000);
                    continue;
                }
                peek[pn] = '\0';
                if (!memchr(peek, '\n', (size_t)pn)) {
                    if (++peek_retries > 4000) goto done;
                    usleep(1000);
                    continue;
                }
                peek_retries = 0;
                char owner[HTTPMIGRATE_TARGET_LEN];
                if (!parse_owner(peek, (size_t)pn, owner, sizeof(owner))) goto done;
                if (strcmp(owner, own) != 0) {
                    relay_https(fd, ssl, pl, owner, relay_sock);
                    ssl = NULL; fd = -1;
                    goto done;
                }
            }
        }
        first = 0;

        /* Read a full request (headers + body) via wolfSSL_read. */
        ssize_t hdr_end = -1;
        size_t hdr_sep = 0, hdr_sz = 0, body_target = 0;
        int sc = 0, framed = 0;
        for (;;) {
            if (hdr_end < 0) {
                ssize_t e = find_subseq(buf, len, "\r\n\r\n");
                if (e >= 0) { hdr_end = e; hdr_sep = 4; }
                else { e = find_subseq(buf, len, "\n\n"); if (e >= 0) { hdr_end = e; hdr_sep = 2; } }
                if (hdr_end >= 0) {
                    hdr_sz = (size_t)hdr_end + hdr_sep;
                    long long cl = parse_cl((const char *)buf, hdr_sz);
                    body_target = (cl < 0) ? 0 : (size_t)cl;
                    sc = has_close((const char *)buf, hdr_sz);
                }
            }
            if (hdr_end >= 0 && len >= hdr_sz + body_target) { framed = 1; break; }
            if (ensure_cap(&buf, &cap, len + 4096 + 1) != 0) goto done;
            int n = wolfSSL_read(ssl, (char *)(buf + len), (int)(cap - len - 1));
            if (n <= 0) goto done;
            len += (size_t)n;
            buf[len] = '\0';
        }
        if (!framed) goto done;

        /* [MICROBENCH] top2 = the container has read ALL bytes of the first
         * migrated request. Migration end-to-end cost = top2 - top1, where top1
         * was stamped by the gateway on the first bytes seen in the socket. */
        if (mb_is_first && pl->base.top1_set) {
            fprintf(stderr, "[MICROBENCH] migration_ns=%llu\n",
                    (unsigned long long)(now_ns() - pl->base.top1_rdtsc));
        }

        size_t req_sz = hdr_sz + body_target;

        /* Business logic in Go, then write the encrypted response. */
        unsigned char *resp = NULL;
        int resp_len = 0;
        if (goInvokeHandler(h, buf, (int)req_sz, sc, &resp, &resp_len) != 0) goto done;
        int off = 0;
        while (off < resp_len) {
            /* One TLS record per call (max TLS 1.3 app-data = 2^14). The
             * migrated session corrupts framing if wolfSSL splits a >16 KB
             * response into several records within a single call, so we chunk
             * it ourselves into 16384-byte records. */
            int chunk = resp_len - off;
            if (chunk > 16384) chunk = 16384;
            int n = wolfSSL_write(ssl, (const char *)(resp + off), chunk);
            if (n <= 0) { free(resp); goto done; }
            off += n;
        }
        free(resp);

        if (pipe_fd >= 0) {
            uint64_t ts = now_ns();
            (void)write(pipe_fd, &ts, sizeof(ts));
            close(pipe_fd);
            pipe_fd = -1;
        }
        if (sc) goto done;

        /* Re-export TLS state for the next peek and keep leftover (pipelined). */
        export_serial(ssl, &pl->serial);
        if (len > req_sz) {
            memmove(buf, buf + req_sz, len - req_sz);
            len -= req_sz;
        } else {
            len = 0;
        }
    }

done:
    if (buf) free(buf);
    if (ssl) { wolfSSL_set_quiet_shutdown(ssl, 1); wolfSSL_free(ssl); }
    if (fd >= 0) close(fd);
    if (pipe_fd >= 0) {
        uint64_t ts = now_ns();
        (void)write(pipe_fd, &ts, sizeof(ts));
        close(pipe_fd);
    }
}

/* ── Public entry points ────────────────────────────────────────────────── */

int wd_bridge_init(const char *cert_file, const char *key_file)
{
    if (!cert_file || !cert_file[0] || !key_file || !key_file[0]) {
        /* HTTP-only: still fine, HTTPS connections will be rejected. */
        return 0;
    }
    wolfSSL_Init();
    s_wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!s_wctx) return -1;
    if (wolfSSL_CTX_use_certificate_file(s_wctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(s_wctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfSSL_CTX_free(s_wctx);
        s_wctx = NULL;
        return -1;
    }
    return 0;
}

void wd_serve_conn(int client_fd, int pipe_fd,
                   const unsigned char *payload, int payload_len,
                   const char *own_fn_name, const char *relay_sock,
                   uintptr_t gohandle)
{
    if (payload_len < (int)sizeof(wd_base_t)) {
        if (client_fd >= 0) close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }

    const wd_base_t *base = (const wd_base_t *)payload;

    if (base->magic == HTTPMIGRATE_HTTPS_MAGIC) {
        if (!s_wctx || payload_len < (int)sizeof(wd_https_payload_t)) {
            fprintf(stderr, "[wd-bridge] HTTPS connection but TLS not initialised / short payload\n");
            if (client_fd >= 0) close(client_fd);
            if (pipe_fd >= 0) close(pipe_fd);
            return;
        }
        wd_https_payload_t *pl = malloc(sizeof(*pl));
        if (!pl) {
            if (client_fd >= 0) close(client_fd);
            if (pipe_fd >= 0) close(pipe_fd);
            return;
        }
        memcpy(pl, payload, sizeof(*pl));
        serve_https_conn(client_fd, pipe_fd, pl, own_fn_name, relay_sock, gohandle);
        free(pl);
    } else {
        /* HMKA or unknown → treat as plain HTTP. */
        serve_http_conn(client_fd, pipe_fd, own_fn_name, relay_sock, gohandle);
    }
}
