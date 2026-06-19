/*
 * timing_fn_ka_worker.c — Keep-Alive HTTP integration worker
 *
 * Integration variant of the micro-bench3-keepalive-http worker.
 * Receives connections from the real faasd/of-watchdog stack via
 * SCM_RIGHTS over a Unix domain socket.
 *
 * Key differences from the standalone prototype:
 *  - Socket path is IP-based, not function-name-based:
 *      <SENDFD_SOCKET_DIR>/<own-ip>-fn.sock
 *  - Receives 2 FDs per connection (clientFD + pipeWriteFD).
 *  - Timing uses clock_gettime(CLOCK_MONOTONIC_RAW), not rdtsc.
 *    top1_rdtsc / top2_rdtsc are nanoseconds; cntfrq = 1_000_000_000.
 *  - After each response, writes 8 bytes (uint64 ns timestamp) into
 *    pipeWriteFD and closes it so the gateway relay can notify Prometheus.
 *  - Relay sends only clientFD (1 FD) via sendfd_with_state().
 *  - Relay socket: <SENDFD_SOCKET_DIR>/<own-ip>-relay.sock
 *
 * Environment variables:
 *   HTTPMIGRATE_KA_FUNCTION_NAME   this function's name (default "timing-fn-b")
 *   SENDFD_SOCKET_DIR              shared socket directory (default /run/tlsmigrate)
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "sendfd.h"
#include "unix_socket.h"

/* ── Timing: nanoseconds via CLOCK_MONOTONIC_RAW ─────────────────────────── */

static uint64_t get_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            perror("[fn-worker] clock_gettime");
            return 0;
        }
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#define CNTFRQ 1000000000ULL

/* ── Payload (must match gateway's KAPayload struct) ─────────────────────── */

#define HTTPMIGRATE_MAGIC   0x484D4B41U   /* 'HMKA' */
#define HTTPMIGRATE_VERSION 2U
#define HTTPMIGRATE_TARGET_LEN 128

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;   /* nanoseconds in integration bench */
    uint64_t cntfrq;       /* always 1_000_000_000             */
    uint8_t  top1_set;
    uint8_t  _pad[7];
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} httpmigrate_ka_payload_t;

/* ── 2-FD receive (gateway → watchdog → function direction) ──────────────── */

static int recvfd2_with_state(int unix_sock,
                              int *fd1_out, int *fd2_out,
                              void *payload, size_t payload_len)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 2)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct iovec iov = {
        .iov_base = payload,
        .iov_len  = payload_len,
    };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t n = recvmsg(unix_sock, &msg, 0);
    if (n <= 0) {
        perror("[fn-worker] recvmsg");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "[fn-worker] recvmsg: no SCM_RIGHTS\n");
        return -1;
    }

    size_t nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    int *fds = (int *)CMSG_DATA(cmsg);

    if (nfds < 2) {
        fprintf(stderr, "[fn-worker] recvmsg: expected 2 FDs, got %zu\n", nfds);
        for (size_t i = 0; i < nfds; i++) close(fds[i]);
        return -1;
    }

    *fd1_out = fds[0]; /* clientFD   */
    *fd2_out = fds[1]; /* pipeWriteFD */

    /* Close any excess FDs (should not happen). */
    for (size_t i = 2; i < nfds; i++) close(fds[i]);

    return 0;
}

/* ── Container IP discovery ──────────────────────────────────────────────── */

static int get_container_ip(char *buf, size_t bufsz)
{
    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0) return -1;

    int found = 0;
    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, (socklen_t)bufsz)) {
            found = 1;
            break;
        }
    }
    freeifaddrs(ifa_list);
    return found ? 0 : -1;
}

/* ── HTTP parsing helpers ────────────────────────────────────────────────── */

static ssize_t find_subseq(const unsigned char *buf, size_t len,
                            const char *needle)
{
    const size_t nlen = strlen(needle);
    if (!nlen || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static long long parse_content_length(const char *headers, size_t hlen)
{
    const char *p = headers, *end = headers + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *needle = "content-length:";
        size_t nlen = strlen(needle);
        if (ll >= nlen) {
            int m = 1;
            for (size_t k = 0; k < nlen && m; k++) {
                char c = p[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
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
        if (name[i] == '/' || name[i] == '?' || name[i] == ' ') {
            nlen = i;
            break;
        }
    if (!nlen || nlen + 1 > outsz) return false;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
    return true;
}

static int wait_readable(int fd)
{
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd     = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
#ifdef POLLRDHUP
    pfd.events |= POLLRDHUP;
#endif
    for (;;) {
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        int avail = 0;
        if (ioctl(fd, FIONREAD, &avail) == 0 && avail > 0) return 1;
        if (pfd.revents & (POLLHUP | POLLERR
#ifdef POLLRDHUP
                           | POLLRDHUP
#endif
                          ))
            return 0;
        unsigned char byte;
        ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) return 1;
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return -1;
    }
}

/* ── Session loop ────────────────────────────────────────────────────────── */

/*
 * process_session — serve keep-alive requests on client_fd.
 *
 * pipe_fd: the write end of the gateway's notification pipe.
 *   - For the FIRST request: we own one pipe_fd.  After we respond we write
 *     8 bytes (uint64 nanosecond timestamp) and close it so the gateway's
 *     Prometheus notifier fires.
 *   - For subsequent requests: we call back into the gateway relay on a fresh
 *     connection; the gateway creates a new pipe and sends us a new pipeWriteFD
 *     with each batch.  However, since we hold the TCP socket and process
 *     subsequent keepalive requests ourselves, the simple model is:
 *     * Write to current pipe_fd after the first response, then keep pipe_fd = -1.
 *     * For subsequent requests the Prometheus pipe is not tracked here (the
 *       relay goroutine creates a fresh pipe each time it re-dispatches).
 *       This matches the original prototype bench behaviour.
 */
static void process_session(int client_fd,
                            int pipe_fd,
                            httpmigrate_ka_payload_t *payload,
                            const char *function_name,
                            const char *relay_socket)
{
    uint64_t req_no = 0;
    bool first_request = true;
    bool pipe_sent = false;

    unsigned char *buf = NULL;
    size_t cap = 0;

    for (;;) {
        req_no++;

        uint64_t top1, cntfrq_stamp;

        if (first_request && payload->top1_set) {
            top1         = payload->top1_rdtsc;
            cntfrq_stamp = payload->cntfrq ? payload->cntfrq : CNTFRQ;
            first_request = false;
        } else {
            /* Wait for the next request on the persistent connection. */
            int wrc = wait_readable(client_fd);
            if (wrc <= 0) goto done;

            /* Stamp top1 before peeking the next request line. */
            top1         = get_ns();
            cntfrq_stamp = CNTFRQ;

            unsigned char peek_buf[1024];
            ssize_t pn = recv(client_fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
            if (pn <= 0) goto done;
            peek_buf[pn] = '\0';

            char owner[HTTPMIGRATE_TARGET_LEN];
            if (!parse_request_owner(peek_buf, (size_t)pn, owner, sizeof(owner)))
                goto done;

            if (strcmp(owner, function_name) != 0) {
                /*
                 * Wrong owner: relay the clientFD (1 FD only) to the gateway
                 * relay socket with the fresh top1 and target function set.
                 */
                httpmigrate_ka_payload_t relay_payload;
                memset(&relay_payload, 0, sizeof(relay_payload));
                relay_payload.magic      = HTTPMIGRATE_MAGIC;
                relay_payload.version    = HTTPMIGRATE_VERSION;
                relay_payload.top1_rdtsc = top1;
                relay_payload.cntfrq     = cntfrq_stamp;
                relay_payload.top1_set   = 1;
                snprintf(relay_payload.target_function,
                         sizeof(relay_payload.target_function), "%s", owner);

                int relay_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
                bool relayed = false;
                if (relay_fd >= 0) {
                    struct sockaddr_un addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sun_family = AF_UNIX;
                    strncpy(addr.sun_path, relay_socket, sizeof(addr.sun_path) - 1);
                    if (connect(relay_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        /* sendfd_with_state sends 1 FD only — closes client_fd */
                        sendfd_with_state(relay_fd, client_fd, &relay_payload,
                                          sizeof(relay_payload));
                        relayed = true;
                    }
                    close(relay_fd);
                }
                if (!relayed) {
                    close(client_fd);
                }
                /* Our session ends here; client_fd already closed. */
                if (buf) free(buf);
                return;
            }
        }

        /* ── Owner matches: serve this request ────────────────────────── */

        size_t total    = 0;
        ssize_t hdr_end = -1;
        size_t hdr_sep  = 0;

        while (hdr_end < 0) {
            if (cap < total + 4096 + 1) {
                size_t nc = cap ? cap * 2 : 16384;
                unsigned char *nb = realloc(buf, nc);
                if (!nb) goto done;
                buf = nb;
                cap = nc;
            }
            ssize_t n = recv(client_fd, buf + total, cap - total - 1, 0);
            if (n <= 0) goto done;
            total += (size_t)n;
            buf[total] = '\0';

            ssize_t e4 = find_subseq(buf, total, "\r\n\r\n");
            if (e4 >= 0) { hdr_end = e4; hdr_sep = 4; break; }
            ssize_t e2 = find_subseq(buf, total, "\n\n");
            if (e2 >= 0) { hdr_end = e2; hdr_sep = 2; break; }
        }

        size_t hdr_sz = (size_t)hdr_end + hdr_sep;
        long long cl  = parse_content_length((const char *)buf, hdr_sz);
        if (cl < 0) cl = 0;

        int should_close = 0;
        if (find_subseq(buf, hdr_sz, "Connection: close") >= 0 ||
            find_subseq(buf, hdr_sz, "connection: close") >= 0)
            should_close = 1;

        size_t body_read = (total > hdr_sz) ? (total - hdr_sz) : 0;
        while (body_read < (size_t)cl) {
            unsigned char scratch[16384];
            size_t want = (size_t)cl - body_read;
            if (want > sizeof(scratch)) want = sizeof(scratch);
            ssize_t n = recv(client_fd, scratch, want, 0);
            if (n <= 0) goto done;
            body_read += (size_t)n;
        }

        /* Stamp top2 after consuming the full body. */
        uint64_t top2    = get_ns();
        uint64_t freq    = cntfrq_stamp ? cntfrq_stamp : CNTFRQ;
        uint64_t delta   = (top2 > top1) ? (top2 - top1) : 0;
        uint64_t delta_ns = delta; /* units are already nanoseconds */

        /* Build JSON response. */
        char json[512];
        int jl = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"integration-http-ka\""
            ",\"top1_rdtsc\":%" PRIu64
            ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_cycles\":%" PRIu64
            ",\"cntfrq\":%" PRIu64
            ",\"delta_ns\":%" PRIu64
            ",\"body_bytes_read\":%zu"
            ",\"content_length\":%lld}",
            function_name, req_no,
            top1, top2, delta, freq, delta_ns,
            body_read, cl);

        char resp[768];
        int rl = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "\r\n%s",
            jl,
            should_close ? "close" : "keep-alive",
            json);

        if (write(client_fd, resp, (size_t)rl) < 0) goto done;

        /* Notify the gateway's Prometheus relay via the pipe. */
        if (!pipe_sent && pipe_fd >= 0) {
            uint64_t ts_le = top2; /* little-endian on amd64/arm64 */
            (void)write(pipe_fd, &ts_le, sizeof(ts_le));
            close(pipe_fd);
            pipe_fd   = -1;
            pipe_sent = true;
        }

        if (should_close) goto done;
    }

done:
    if (buf) free(buf);
    close(client_fd);
    if (!pipe_sent && pipe_fd >= 0) {
        /* Ensure gateway relay goroutine unblocks even on error. */
        uint64_t ts_le = get_ns();
        (void)write(pipe_fd, &ts_le, sizeof(ts_le));
        close(pipe_fd);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    const char *fn_name    = getenv("HTTPMIGRATE_KA_FUNCTION_NAME");
    const char *socket_dir = getenv("SENDFD_SOCKET_DIR");

    if (!fn_name    || !fn_name[0])    fn_name    = "timing-fn-b";
    if (!socket_dir || !socket_dir[0]) socket_dir = "/run/tlsmigrate";

    /* Discover own container IP. */
    char own_ip[INET_ADDRSTRLEN];
    if (get_container_ip(own_ip, sizeof(own_ip)) != 0) {
        fprintf(stderr, "[fn-worker] failed to discover container IP\n");
        return 1;
    }
    fprintf(stderr, "[fn-worker] own IP = %s\n", own_ip);

    /* Socket paths. */
    char fn_sock_path[256];
    snprintf(fn_sock_path, sizeof(fn_sock_path), "%s/%s-fn.sock",
             socket_dir, own_ip);

    char relay_sock_path[256];
    /* Provider-routed path: workers return wrong-owner keep-alive connections to
       the single faasd provider socket, which re-routes to the correct container. */
    snprintf(relay_sock_path, sizeof(relay_sock_path), "%s/provider.sock",
             socket_dir);

    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "[fn-worker] warning: mkdir %s: %s\n",
                socket_dir, strerror(errno));

    unlink(fn_sock_path);
    int listen_fd = unix_server_socket(fn_sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[fn-worker] unix_server_socket(%s) failed: %s\n",
                fn_sock_path, strerror(errno));
        return 1;
    }
    chmod(fn_sock_path, 0777);

    fprintf(stderr, "[fn-worker] %s listening on %s, relay=%s\n",
            fn_name, fn_sock_path, relay_sock_path);

    for (;;) {
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) continue;

        httpmigrate_ka_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        int client_fd  = -1;
        int pipe_wr_fd = -1;

        if (recvfd2_with_state(conn_fd, &client_fd, &pipe_wr_fd,
                               &payload, sizeof(payload)) != 0) {
            close(conn_fd);
            continue;
        }
        close(conn_fd);

        if (client_fd < 0) {
            if (pipe_wr_fd >= 0) close(pipe_wr_fd);
            continue;
        }
        if (payload.magic != HTTPMIGRATE_MAGIC ||
            payload.version != HTTPMIGRATE_VERSION) {
            fprintf(stderr, "[fn-worker] bad magic 0x%08x\n", payload.magic);
            close(client_fd);
            if (pipe_wr_fd >= 0) close(pipe_wr_fd);
            continue;
        }

        /* Make TCP fd blocking. */
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

        process_session(client_fd, pipe_wr_fd, &payload,
                        fn_name, relay_sock_path);
    }
}
