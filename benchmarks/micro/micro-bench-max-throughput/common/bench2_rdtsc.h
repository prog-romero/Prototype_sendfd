/*
 * bench2_rdtsc.h — Monotonic timestamp and FIONREAD busy-poll helpers.
 *
 * For now this benchmark uses current monotonic time in nanoseconds instead of
 * rdtsc/CNTVCT-based counters. The existing bench2_rdtsc()/bench2_cntfrq()
 * names are kept so the rest of the benchmark code and CSV schema stay intact.
 *
 * names are kept so older benchmark helpers can still build unchanged.
 */
#ifndef BENCH2_RDTSC_H
#define BENCH2_RDTSC_H

#include <stdint.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>

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
 * bench2_wait_readable(fd) — Spin-yield until at least one byte is present
 * in fd's kernel receive buffer.
 *
 * Must be called on the raw TCP socket fd (not on a wolfSSL handle, not on a
 * Unix-domain socket).
 */
static inline void bench2_wait_readable(int fd)
{
    int avail = 0;
    do {
        ioctl(fd, FIONREAD, &avail);
        if (avail > 0) return;
        sched_yield();
    } while (1);
}

#endif /* BENCH2_RDTSC_H */
