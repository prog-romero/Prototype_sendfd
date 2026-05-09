/*
 * bench2_rdtsc.h — Portable cycle-counter and FIONREAD busy-poll helpers.
 *
 * On x86-64  : uses the RDTSC instruction.
 * On AArch64 : uses the CNTVCT_EL0 architectural virtual timer register,
 *              which is readable from user-space (EL0) and is globally
 *              synchronised across all cores.
 */
#ifndef BENCH2_RDTSC_H
#define BENCH2_RDTSC_H

#include <stdint.h>
#include <sched.h>
#include <sys/ioctl.h>

static inline uint64_t bench2_rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
#  error "bench2_rdtsc: unsupported architecture (need x86_64 or aarch64)"
#endif
}

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

static inline void bench2_wait_readable(int fd)
{
    int avail = 0;
    do {
        ioctl(fd, FIONREAD, &avail);
        if (avail > 0) {
            return;
        }
        sched_yield();
    } while (1);
}

#endif /* BENCH2_RDTSC_H */