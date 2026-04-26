package benchhttps

/*
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>

static inline uint64_t bench2_go_rdtsc(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t bench2_go_cntfrq(void) {
    return 1000000000ULL;
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

func benchWaitReadable(fd int) {
	C.bench2_go_wait_readable(C.int(fd))
}

func benchWaitReadableTimeout(fd int, timeoutMs int) bool {
	return C.bench2_go_wait_readable_timeout(C.int(fd), C.int(timeoutMs)) != 0
}
