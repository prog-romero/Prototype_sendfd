package benchhttps

/*
#include <stdint.h>
#include <errno.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

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

// Block with epoll until fd is readable (no busy spin, no poll).
static inline void bench2_go_wait_readable(int fd) {
    int epfd = epoll_create1(0);
    if (epfd < 0) return;
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) { close(epfd); return; }
    struct epoll_event out;
    for (;;) {
        int n = epoll_wait(epfd, &out, 1, -1);
        if (n > 0) break;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(epfd);
}

// Block with epoll up to timeout_ms ms; returns 1 if readable, 0 otherwise.
static inline int bench2_go_wait_readable_timeout(int fd, int timeout_ms) {
    int epfd = epoll_create1(0);
    if (epfd < 0) return 0;
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) { close(epfd); return 0; }
    struct epoll_event out;
    int ready = 0;
    for (;;) {
        int n = epoll_wait(epfd, &out, 1, timeout_ms);
        if (n > 0) { ready = 1; break; }
        if (n == 0) break;  // timeout
        if (errno == EINTR) continue;
        break;
    }
    close(epfd);
    return ready;
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
