package benchhttp

/*
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>

static inline uint64_t bench2_go_rdtsc(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
# error "bench2_go_rdtsc: unsupported architecture"
#endif
}

static inline uint64_t bench2_go_cntfrq(void) {
#if defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
#else
    return 1000000000ULL;
#endif
}

static inline int bench2_go_pending_bytes(int fd) {
    int avail = 0;
    if (ioctl(fd, FIONREAD, &avail) != 0) {
        return 0;
    }
    return avail;
}

static inline void bench2_go_wait_readable(int fd) {
    int avail = 0;
    do {
        ioctl(fd, FIONREAD, &avail);
        if (avail > 0) {
            return;
        }
        sched_yield();
    } while (1);
}

static inline int bench2_go_wait_readable_timeout(int fd, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc >= 0) {
            return rc > 0;
        }
        if (errno != EINTR) {
            return 0;
        }
    }
}
*/
import "C"

func benchReadCounter() uint64 {
	return uint64(C.bench2_go_rdtsc())
}

func benchCounterFreq() uint64 {
	return uint64(C.bench2_go_cntfrq())
}

func benchPendingBytes(fd int) int {
	return int(C.bench2_go_pending_bytes(C.int(fd)))
}

func benchWaitReadable(fd int) {
	C.bench2_go_wait_readable(C.int(fd))
}

func benchWaitReadableTimeout(fd int, timeoutMs int) bool {
	return C.bench2_go_wait_readable_timeout(C.int(fd), C.int(timeoutMs)) != 0
}
