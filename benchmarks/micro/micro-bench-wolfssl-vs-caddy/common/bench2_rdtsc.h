/*
 * bench2_rdtsc.h -- Portable cycle-counter helper copied locally so this
 * benchmark stays standalone.
 */
#ifndef BENCH2_RDTSC_H
#define BENCH2_RDTSC_H

#include <stdint.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>

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

static inline uint64_t bench2_cntfrq(void)
{
    return 1000000000ULL;
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