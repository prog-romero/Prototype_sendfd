/*
 * bench2_rdtsc.h — Monotonic timestamp and FIONREAD busy-poll helpers.
 *
 * For now this benchmark uses current monotonic time in nanoseconds instead of
 * rdtsc/CNTVCT-based counters. The existing bench2_rdtsc()/bench2_cntfrq()
 * names are kept so the rest of the benchmark code and CSV schema stay intact.
 *
 * Usage pattern:
 *
 *   bench2_wait_readable(fd);            // spin-yield until FIONREAD > 0
 *   uint64_t top1 = bench2_rdtsc();      // stamp — data is already in buffer
 *   ... perform read/wolfSSL_read ...
 *   uint64_t top2 = bench2_rdtsc();      // stamp — after full payload read
 *   uint64_t delta = top2 - top1;        // already nanoseconds
 *   uint64_t freq  = bench2_cntfrq();    // fixed 1 GHz scale
 */
#ifndef BENCH2_RDTSC_H
#define BENCH2_RDTSC_H

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ─── Monotonic timestamp helper ───────────────────────────────────────── */

/*
 * bench2_rdtsc() — Read a monotonic nanosecond timestamp.
 */
static inline uint64_t bench2_rdtsc(void)
{
    struct timespec ts;

#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * bench2_cntfrq() — Timestamp ticks per second.
 */
static inline uint64_t bench2_cntfrq(void)
{
    return 1000000000ULL;
}

/* ─── FIONREAD busy-poll ─────────────────────────────────────────────────── */

/*
 * bench2_wait_readable(fd) — Block (epoll) until at least one byte is present
 * in fd's kernel receive buffer. Uses epoll_wait so the caller does not burn
 * CPU while waiting. Call immediately before bench2_rdtsc() so that top1 is
 * stamped only when data is already sitting in the kernel buffer.
 */
static inline void bench2_wait_readable(int fd)
{
    int epfd = epoll_create1(0);
    if (epfd < 0) return;  /* fallback: proceed immediately */

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        close(epfd);
        return;
    }

    struct epoll_event out;
    for (;;) {
        int n = epoll_wait(epfd, &out, 1, -1);
        if (n > 0) break;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(epfd);
}

#endif /* BENCH2_RDTSC_H */
