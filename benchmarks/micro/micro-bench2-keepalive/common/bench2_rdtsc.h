/*
 * bench2_rdtsc.h — Portable cycle-counter and FIONREAD busy-poll helpers.
 *
 * On x86-64  : uses the RDTSC instruction.
 * On AArch64 : uses the CNTVCT_EL0 architectural virtual timer register,
 *              which is readable from user-space (EL0) and is globally
 *              synchronised across all cores (unlike TSC on multi-socket x86).
 *              Its frequency is given by CNTFRQ_EL0 (e.g. 54 MHz on RPi 4).
 *
 * Usage pattern:
 *
 *   bench2_wait_readable(fd);            // spin-yield until FIONREAD > 0
 *   uint64_t top1 = bench2_rdtsc();      // stamp — data is already in buffer
 *   ... perform read/wolfSSL_read ...
 *   uint64_t top2 = bench2_rdtsc();      // stamp — after full payload read
 *   uint64_t delta = top2 - top1;
 *   uint64_t freq  = bench2_cntfrq();    // ticks per second
 *   double   ns    = (double)delta * 1.0e9 / (double)freq;
 */
#ifndef BENCH2_RDTSC_H
#define BENCH2_RDTSC_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sched.h>

/* ─── Cycle / virtual-timer counter ────────────────────────────────────── */

/*
 * bench2_rdtsc() — Read the CPU or architectural timer counter.
 *
 * The value is monotonically increasing; the delta between two readings in
 * the same process (or across processes on arm64) gives precise elapsed time.
 */
static inline uint64_t bench2_rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    /* ISB ensures prior memory operations complete before the timer read. */
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
#  error "bench2_rdtsc: unsupported architecture (need x86_64 or aarch64)"
#endif
}

/*
 * bench2_cntfrq() — Counter ticks per second.
 *
 * On AArch64: reads CNTFRQ_EL0. On x86: returns a 1 GHz placeholder
 * (accurate calibration out of scope for this benchmark).
 */
static inline uint64_t bench2_cntfrq(void)
{
#if defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
#else
    return 1000000000ULL;
#endif
}

/* ─── FIONREAD busy-poll ─────────────────────────────────────────────────── */

/*
 * bench2_wait_readable(fd) — Spin-yield until at least one byte is present
 * in fd's kernel receive buffer.
 *
 * Must be called on the raw TCP socket fd (not on a wolfSSL handle, not on a
 * Unix-domain socket).  Call immediately before bench2_rdtsc() so that top1
 * is stamped only when the data is already sitting in the kernel buffer,
 * with no network propagation delay mixed into the measurement.
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
