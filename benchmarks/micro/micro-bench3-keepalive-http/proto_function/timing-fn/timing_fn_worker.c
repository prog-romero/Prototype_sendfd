#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "sendfd.h"
#include "unix_socket.h"
#include "bench2_rdtsc.h"

#define HTTPMIGRATE_MAGIC 0x484D4754U
#define HTTPMIGRATE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t top1_set;
    uint8_t _pad[7];
} httpmigrate_payload_t;

/* ---------- helpers unchanged ---------- */

static ssize_t find_subseq(const unsigned char *buf, size_t len, const char *needle) {
    const size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) return (ssize_t)i;
    }
    return -1;
}

static long long parse_content_length(const char *headers, size_t headers_len) {
    const char *p = headers;
    const char *end = headers + headers_len;
    const char *needle = "content-length:";

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        else line_end++;

        const char *line = p;
        while (line < line_end && (*line == '\r' || *line == '\n')) line++;

        if (line < line_end) {
            const char *found = strcasestr(line, needle);
            if (found == line) {
                const char *v = line + strlen(needle);
                while (v < line_end && (*v == ' ' || *v == '\t')) v++;
                char *num_end = NULL;
                errno = 0;
                long long cl = strtoll(v, &num_end, 10);
                if (errno == 0 && num_end && num_end > v && cl >= 0) return cl;
            }
        }
        p = line_end;
    }
    return -1;
}

static int ensure_capacity(unsigned char **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return 0;
    size_t new_cap = *cap ? *cap : 8192;
    while (new_cap < needed) new_cap *= 2;
    unsigned char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return -1;
    *buf = new_buf;
    *cap = new_cap;
    return 0;
}

/* ---------- request handling unchanged ---------- */

static int handle_one_request(int client_fd, const httpmigrate_payload_t *payload) {
    unsigned char *buf = NULL;
    size_t cap = 0;
    size_t total = 0;
    size_t body_bytes_read = 0;
    long long content_length = 0;

    ssize_t header_end = -1;
    size_t header_sep_len = 0;

    while (header_end < 0) {
        if (ensure_capacity(&buf, &cap, total + 4096 + 1) != 0) goto out;
        fprintf(stderr, "[worker] reading headers... (total=%zu)\n", total);
        ssize_t n = read(client_fd, buf + total, cap - total - 1);
        if (n <= 0) {
            fprintf(stderr, "[worker] header read failed: %zd (errno=%d)\n", n, errno);
            goto out;
        }
        total += (size_t)n;
        buf[total] = '\0';

        header_end = find_subseq(buf, total, "\r\n\r\n");
        if (header_end >= 0) { header_sep_len = 4; break; }
        header_end = find_subseq(buf, total, "\n\n");
        if (header_end >= 0) { header_sep_len = 2; break; }
    }

    const size_t headers_len = (size_t)header_end + header_sep_len;
    content_length = parse_content_length((const char *)buf, headers_len);
    if (content_length < 0) content_length = 0;

    fprintf(stderr, "[worker] headers found (len=%zu), Content-Length: %lld\n", headers_len, content_length);

    size_t body_start = headers_len;
    if (total > body_start) {
        size_t already = total - body_start;
        if (already > (size_t)content_length) already = (size_t)content_length;
        body_bytes_read += already;
        fprintf(stderr, "[worker] body bytes already in buffer: %zu\n", already);
    }

    while (body_bytes_read < (size_t)content_length) {
        unsigned char scratch[16384];
        size_t want = (size_t)content_length - body_bytes_read;
        if (want > sizeof(scratch)) want = sizeof(scratch);
        fprintf(stderr, "[worker] reading body... (want=%zu)\n", want);
        ssize_t n = read(client_fd, scratch, want);
        if (n <= 0) {
            fprintf(stderr, "[worker] body read failed: %zd (errno=%d)\n", n, errno);
            goto out;
        }
        body_bytes_read += (size_t)n;
    }
    fprintf(stderr, "[worker] request fully read (body=%zu)\n", body_bytes_read);

    const uint64_t top2 = bench2_rdtsc();
    const uint64_t freq = bench2_cntfrq();
    const uint64_t top1 = payload->top1_set ? payload->top1_rdtsc : 0;
    const uint64_t cntfrq = payload->cntfrq ? payload->cntfrq : freq;
    const uint64_t delta = (top2 > top1) ? (top2 - top1) : 0;
    const uint64_t delta_ns = (cntfrq > 0)
        ? (uint64_t)(((long double)delta * 1000000000.0L) / (long double)cntfrq)
        : 0;

    char json[512];
    int json_len = snprintf(json, sizeof(json),
        "{\"path\":\"prototype-http\",\"top1_rdtsc\":%" PRIu64 ",\"top2_rdtsc\":%" PRIu64
        ",\"delta_cycles\":%" PRIu64 ",\"cntfrq\":%" PRIu64 ",\"delta_ns\":%" PRIu64
        ",\"body_bytes_read\":%zu,\"content_length\":%lld}",
        top1, top2, delta, cntfrq, delta_ns, body_bytes_read, content_length);

    char resp[1024];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        json_len, json);

    write(client_fd, resp, (size_t)resp_len);
    free(buf);
    return 0;

out:
    if (buf) free(buf);
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);

    /* Read socket dir from env, default /run/httpmigrate.
     * faasd bind-mounts /var/lib/faasd/httpmigrate -> /run/httpmigrate
     * inside both the gateway container (via docker-compose.yaml volumes)
     * and the function container (faasd-provider mirrors ./httpmigrate mounts).
     * This is the same mechanism as ./tlsmigrate in micro-bench2-initale-time. */
    const char *socket_dir = getenv("HTTPMIGRATE_SOCKET_DIR");
    if (!socket_dir || socket_dir[0] == '\0') socket_dir = "/run/httpmigrate";

    const char *function_name = getenv("HTTPMIGRATE_FUNCTION_NAME");
    if (!function_name || function_name[0] == '\0')
        function_name = "timing-fn";

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", socket_dir, function_name);

    /* Ensure permissive socket permissions for cross-container access */
    umask(0);
    if (mkdir(socket_dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[http-worker] warning: mkdir(%s) failed: %s\n",
                socket_dir, strerror(errno));
    } else {
        (void)chmod(socket_dir, 0777);
    }

    /* Remove stale socket from a previous run */
    unlink(sock_path);

    int listen_fd = unix_server_socket(sock_path, 4096);
    if (listen_fd < 0) {
        fprintf(stderr, "[http-worker] unix_server_socket(%s) failed\n", sock_path);
        return 1;
    }

    (void)chmod(sock_path, 0777);

    fprintf(stderr, "[http-worker] listening on %s (Zero-Copy HTTP mode)\n", sock_path);

    for (;;) {
        int conn_fd = unix_accept(listen_fd);
        if (conn_fd < 0) continue;

        httpmigrate_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        int client_fd = -1;

        if (recvfd_with_state(conn_fd, &client_fd, &payload, sizeof(payload)) != 0) {
            close(conn_fd);
            continue;
        }
        close(conn_fd);

        if (client_fd >= 0) {
            /* Go's net/http sets accepted sockets to non-blocking. 
             * We must clear O_NONBLOCK so our standard blocking read() loop doesn't fail with EAGAIN on large payloads. */
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
            }

            if (payload.magic == HTTPMIGRATE_MAGIC &&
                payload.version == HTTPMIGRATE_VERSION) {
                (void)handle_one_request(client_fd, &payload);
            } else {
                fprintf(stderr, "[http-worker] bad payload magic: 0x%08x\n", payload.magic);
            }
            close(client_fd);
        }
    }

    return 0;
}
