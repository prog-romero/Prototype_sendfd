/*
 * httpmigrate_ka.c — Keep-Alive HTTP gateway shim
 *
 * Extends bench2-initial-http with:
 *   - extended payload carrying target_function name
 *   - relay Unix socket to receive FDs back from wrong-owner workers
 */
#include "httpmigrate_ka.h"

#include "sendfd.h"
#include "unix_socket.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Timing ─────────────────────────────────────────────────────────────── */

static uint64_t bench_rdtsc(void) {
#if defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

static uint64_t bench_cntfrq(void) {
#if defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
#else
    return 1000000000ULL;
#endif
}

static void wait_readable(int fd) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    poll(&pfd, 1, 5000);
}

/* ── httpmigrate_ka_accept_peek ─────────────────────────────────────────── */

int httpmigrate_ka_accept_peek(
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    httpmigrate_ka_payload_t* payload_out)
{
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return -1;

    wait_readable(client_fd);

    uint64_t t1   = bench_rdtsc();
    uint64_t freq = bench_cntfrq();

    ssize_t n = recv(client_fd, headers_buf, headers_buf_sz - 1, MSG_PEEK);
    if (n <= 0) {
        close(client_fd);
        return -1;
    }
    headers_buf[n] = '\0';

    if (headers_len_out) *headers_len_out = (int)n;

    memset(payload_out, 0, sizeof(*payload_out));
    payload_out->magic      = HTTPMIGRATE_MAGIC;
    payload_out->version    = HTTPMIGRATE_VERSION;
    payload_out->top1_rdtsc = t1;
    payload_out->cntfrq     = freq;
    payload_out->top1_set   = 1;

    return client_fd;
}

/* ── httpmigrate_ka_send_fd ─────────────────────────────────────────────── */

int httpmigrate_ka_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const httpmigrate_ka_payload_t* payload,
    size_t payload_len)
{
    int uds_fd = unix_client_connect(unix_sock_path);
    if (uds_fd < 0) { close(client_fd); return -1; }

    int rc = sendfd_with_state(uds_fd, client_fd, payload, payload_len);
    close(uds_fd);
    if (rc != 0) { close(client_fd); }
    return rc;
}

/* ── httpmigrate_ka_unix_server ─────────────────────────────────────────── */

int httpmigrate_ka_unix_server(const char* sock_path)
{
    int fd = unix_server_socket(sock_path, 4096);
    if (fd < 0) return -1;
    (void)chmod(sock_path, 0777);
    return fd;
}

/* ── httpmigrate_ka_accept_recv ─────────────────────────────────────────── */

int httpmigrate_ka_accept_recv(int relay_listen_fd,
                               httpmigrate_ka_payload_t* payload_out)
{
    if (relay_listen_fd < 0 || !payload_out) { errno = EINVAL; return -1; }

    int conn_fd = unix_accept(relay_listen_fd);
    if (conn_fd < 0) return -1;

    int client_fd = -1;
    if (recvfd_with_state(conn_fd, &client_fd, payload_out, sizeof(*payload_out)) != 0) {
        close(conn_fd);
        if (client_fd >= 0) close(client_fd);
        return -1;
    }
    close(conn_fd);
    return client_fd;
}
