#include "httpmigrate.h"

#include "sendfd.h"
#include "unix_socket.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

static uint64_t bench_rdtsc(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
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

#include <poll.h>

static void wait_readable(int fd) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    poll(&pfd, 1, 5000); /* 5 second timeout to prevent infinite hang */
}

/* 
 * Returns the end position of the /function/<name> part in headers_buf 
 * using MSG_PEEK to avoid consuming data.
 */
int httpmigrate_accept_peek(
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    httpmigrate_payload_t* payload_out) {

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return -1;

    fprintf(stderr, "[gateway] accepted client_fd=%d, waiting readable...\n", client_fd);
    wait_readable(client_fd);

    /* Stamp Top1 only once request bytes are already available, immediately
     * before the non-consuming peek that routes the request. */
    uint64_t t1 = bench_rdtsc();
    uint64_t freq = bench_cntfrq();

    fprintf(stderr, "[gateway] peeking headers...\n");
    /* Use MSG_PEEK to see the headers WITHOUT removing them from kernel buffer */
    ssize_t n = recv(client_fd, headers_buf, headers_buf_sz - 1, MSG_PEEK);
    if (n <= 0) {
        fprintf(stderr, "[gateway] peek failed: %zd (errno=%d)\n", n, errno);
        close(client_fd);
        return -1;
    }
    headers_buf[n] = '\0';
    fprintf(stderr, "[gateway] peeked %zd bytes\n", n);

    if (headers_len_out) *headers_len_out = (int)n;

    /* Prepare payload for worker */
    memset(payload_out, 0, sizeof(*payload_out));
    payload_out->magic = HTTPMIGRATE_MAGIC;
    payload_out->version = HTTPMIGRATE_VERSION;
    payload_out->top1_rdtsc = t1;
    payload_out->cntfrq = freq;
    payload_out->top1_set = 1;

    return client_fd;
}

int httpmigrate_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const httpmigrate_payload_t* payload,
    size_t payload_len) {

    fprintf(stderr, "[gateway] connecting to worker at %s...\n", unix_sock_path);
    int uds_fd = unix_client_connect(unix_sock_path);
    if (uds_fd < 0) {
        fprintf(stderr, "[gateway] connect failed: %s\n", strerror(errno));
        close(client_fd);
        return -1;
    }

    fprintf(stderr, "[gateway] sending fd=%d...\n", client_fd);
    int rc = sendfd_with_state(uds_fd, client_fd, payload, payload_len);
    close(uds_fd);
    /* Note: sendfd_with_state closes client_fd on success. 
       If it fails, we still close it to avoid leaks. */
    if (rc != 0) {
        fprintf(stderr, "[gateway] sendfd failed: %d\n", rc);
        close(client_fd);
    } else {
        fprintf(stderr, "[gateway] sendfd success\n");
    }

    return rc;
}
