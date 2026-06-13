//go:build linux && cgo

/*
 * tls_listener.go — Go CGo wrapper for the wolfSSL gateway bridge.
 *
 * WolfSSLGtwCtx  — shared wolfSSL context (one per gateway process).
 * WolfSSLGtwConn — net.Conn wrapper for connections on the ChanListener path.
 *                  Used for non-function HTTPS requests (/system/*, /ui/*, …)
 *                  that are served by the standard OpenFaaS HTTP router.
 *
 * The epoll-driven accept/handshake/peek loop is in loop_https.go (runEpollLoop).
 * This file provides only:
 *   - Context creation/destruction.
 *   - The WolfSSLGtwConn net.Conn implementation (Read/Write/Close/deadlines).
 *   - SerialSize() for TLS export buffer allocation.
 *
 * THREADING MODEL:
 *   - One goroutine runs the shared epoll loop (loop_https.go).
 *   - After handshake, the conn is pushed to ChanListener and the http.Server
 *     spawns one goroutine per connection to handle sequential requests.
 *   - Each WolfSSLGtwConn has its own per-conn epoll fd so its goroutine can
 *     suspend (via EpollWait) without blocking other goroutines or OS threads.
 *   - WolfSSLGtwConn.mu protects all fields shared between the goroutine and
 *     the Close() caller (which may come from a different goroutine).
 */
package httpmigrate

/*
#cgo CFLAGS:  -I/usr/local/include -DHAVE_SECRET_CALLBACK -DWOLFSSL_KEYLOG_EXPORT
#cgo LDFLAGS: -L/usr/local/lib -lwolfssl -ltlspeek -lpthread

#include <stdlib.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include "tls_listener.h"
*/
import "C"

import (
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// ── WolfSSLGtwCtx ────────────────────────────────────────────────────────────

// WolfSSLGtwCtx holds the shared wolfSSL context used for all HTTPS connections.
// It is created once per gateway process and freed on shutdown.
type WolfSSLGtwCtx struct {
	ptr *C.wolfssl_gtw_ctx_t
}

// NewWolfSSLGtwCtx creates a wolfSSL context loading the given certificate and
// key files.  Returns an error if wolfSSL init fails or the cert/key are invalid.
func NewWolfSSLGtwCtx(certFile, keyFile string) (*WolfSSLGtwCtx, error) {
	cCert := C.CString(certFile)
	defer C.free(unsafe.Pointer(cCert))
	cKey := C.CString(keyFile)
	defer C.free(unsafe.Pointer(cKey))

	ptr := C.wolfssl_gtw_ctx_new(cCert, cKey)
	if ptr == nil {
		return nil, fmt.Errorf("[tls_listener] wolfssl_gtw_ctx_new failed (cert=%s key=%s)",
			certFile, keyFile)
	}
	return &WolfSSLGtwCtx{ptr: ptr}, nil
}

// Free releases the wolfSSL context.  Must be called when the gateway shuts down.
func (c *WolfSSLGtwCtx) Free() {
	if c == nil || c.ptr == nil {
		return
	}
	C.wolfssl_gtw_ctx_free(c.ptr)
	c.ptr = nil
}

// SerialSize returns the exact byte size of a serialised TLS session state
// (tlspeek_serial_t).  Used to pre-allocate the sendfd payload buffer.
func (c *WolfSSLGtwCtx) SerialSize() int {
	return int(C.TLSGW_SERIAL_SIZE)
}

// ── WolfSSLGtwConn (net.Conn for ChanListener path) ──────────────────────────

// WolfSSLGtwConn is a per-connection handle for the ChanListener path.
// It wraps a *C.wolfssl_gtw_conn_t and implements net.Conn so the standard
// http.Server can use it transparently.
//
// Created by wrapGtwConn() in loop_https.go after the epoll loop completes
// the TLS handshake and determines the request is NOT a /function/ path.
type WolfSSLGtwConn struct {
	ptr           *C.wolfssl_gtw_conn_t
	fd            int
	epfd          int // per-conn epoll fd for Read/Write suspension
	mu            sync.Mutex
	wg            sync.WaitGroup
	closed        bool
	localAddr     net.Addr
	remoteAddr    net.Addr
	readDeadline  time.Time
	writeDeadline time.Time
}

// waitEpoll suspends the calling goroutine until the fd is ready for the
// requested direction (read or write), or until a 500 ms timeout elapses.
//
// The 500 ms timeout is intentional: it ensures the goroutine wakes up to
// re-check the deadline even if no network event arrives (wolfSSL may return
// WANT_READ/WANT_WRITE after a SO_RCVTIMEO-triggered EAGAIN, and EpollWait
// with -1 would block forever in that case).
//
// PERFORMANCE NOTE: evs is declared as a stack array ([1]syscall.EpollEvent)
// rather than a slice (make([]syscall.EpollEvent, 1)) to avoid a heap
// allocation on every call.  At 100+ RPS with keep-alive connections, each
// request triggers multiple Read/Write calls each calling waitEpoll, so the
// allocation adds up quickly.
func (c *WolfSSLGtwConn) waitEpoll(write bool) {
	if c.epfd < 0 || c.fd < 0 {
		time.Sleep(1 * time.Millisecond)
		return
	}

	// Update epoll interest to match the direction we are waiting for.
	var ev syscall.EpollEvent
	ev.Fd = int32(c.fd)
	if write {
		ev.Events = syscall.EPOLLOUT | syscall.EPOLLRDHUP | syscall.EPOLLERR
	} else {
		ev.Events = syscall.EPOLLIN | syscall.EPOLLRDHUP | syscall.EPOLLERR
	}
	_ = syscall.EpollCtl(c.epfd, syscall.EPOLL_CTL_MOD, c.fd, &ev)

	// Stack-allocated event array — zero heap pressure.
	var evs [1]syscall.EpollEvent
	for {
		n, err := syscall.EpollWait(c.epfd, evs[:], 500) // 500 ms max
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			break
		}
		if n > 0 {
			break // fd is ready
		}
		// n == 0: timeout — return so caller can re-check deadline / closed.
		c.mu.Lock()
		cl := c.closed
		c.mu.Unlock()
		if cl {
			break
		}
		break
	}
}

// Read implements net.Conn.  Drives wolfSSL_read() through the non-blocking
// WANT_READ/WANT_WRITE retry loop, checking deadlines on each EAGAIN.
func (c *WolfSSLGtwConn) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	for {
		c.mu.Lock()
		if c.closed || c.ptr == nil {
			c.mu.Unlock()
			return 0, net.ErrClosed
		}
		ptr := c.ptr
		c.wg.Add(1)
		c.mu.Unlock()

		n := C.wolfssl_gtw_conn_read(ptr, unsafe.Pointer(&p[0]), C.int(len(p)))

		c.mu.Lock()
		code := int(C.wolfssl_gtw_conn_get_error(ptr, n))
		c.wg.Done()
		c.mu.Unlock()

		if n > 0 {
			return int(n), nil
		}

		switch code {
		case int(C.SSL_ERROR_WANT_READ):
			// wolfSSL needs more data from the peer.  Check deadline before
			// blocking so we don't wait past an already-expired deadline.
			c.mu.Lock()
			dl := c.readDeadline
			c.mu.Unlock()
			if !dl.IsZero() && time.Now().After(dl) {
				return 0, os.ErrDeadlineExceeded
			}
			c.waitEpoll(false)
			continue

		case int(C.SSL_ERROR_WANT_WRITE):
			// wolfSSL needs to flush its write buffer (TLS handshake message)
			// before it can deliver the next application record.
			c.mu.Lock()
			dl := c.readDeadline
			c.mu.Unlock()
			if !dl.IsZero() && time.Now().After(dl) {
				return 0, os.ErrDeadlineExceeded
			}
			c.waitEpoll(true)
			continue

		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E), int(C.SSL_ERROR_SYSCALL):
			// Peer closed the connection gracefully or abruptly.
			return 0, io.EOF

		default:
			return 0, fmt.Errorf("wolfSSL_read error code=%d", code)
		}
	}
}

// Write implements net.Conn.  Drives wolfSSL_write() through the non-blocking
// WANT_READ/WANT_WRITE retry loop with the SAME buffer on each retry
// (wolfSSL requirement: buffer and length must not change between retries).
func (c *WolfSSLGtwConn) Write(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	total := 0
	for total < len(p) {
		c.mu.Lock()
		if c.closed || c.ptr == nil {
			c.mu.Unlock()
			return total, net.ErrClosed
		}
		ptr := c.ptr
		c.wg.Add(1)
		c.mu.Unlock()

		n := C.wolfssl_gtw_conn_write(ptr, unsafe.Pointer(&p[total]), C.int(len(p)-total))

		c.mu.Lock()
		code := int(C.wolfssl_gtw_conn_get_error(ptr, n))
		c.wg.Done()
		c.mu.Unlock()

		if n > 0 {
			total += int(n)
			continue
		}

		switch code {
		case int(C.SSL_ERROR_WANT_READ):
			// wolfSSL must process a pending TLS 1.3 handshake message from
			// the peer (e.g. KeyUpdate) before it can send application data.
			// The C layer does NOT drain here — it returns the error directly
			// so we can wait on epoll and retry wolfSSL_write() with the same
			// buffer, letting wolfSSL handle the handshake internally.
			c.mu.Lock()
			dl := c.writeDeadline
			c.mu.Unlock()
			if !dl.IsZero() && time.Now().After(dl) {
				return total, os.ErrDeadlineExceeded
			}
			c.waitEpoll(false)
			continue

		case int(C.SSL_ERROR_WANT_WRITE):
			// Socket send buffer full — wait for drain.
			c.mu.Lock()
			dl := c.writeDeadline
			c.mu.Unlock()
			if !dl.IsZero() && time.Now().After(dl) {
				return total, os.ErrDeadlineExceeded
			}
			c.waitEpoll(true)
			continue

		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E), int(C.SSL_ERROR_SYSCALL):
			return total, io.EOF

		default:
			return total, fmt.Errorf("wolfSSL_write error code=%d", code)
		}
	}
	return total, nil
}

// Close implements net.Conn.  Shuts down the TCP socket to unblock any
// concurrent Read/Write, waits for in-flight CGo calls to finish (wg.Wait),
// then frees the wolfSSL session and closes the epoll fd.
func (c *WolfSSLGtwConn) Close() error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return nil
	}
	c.closed = true
	fd := c.fd
	c.mu.Unlock()

	// SHUT_RDWR causes any blocked recv()/send() on the fd to return ENOTCONN,
	// which makes wolfSSL return SSL_ERROR_SYSCALL, which Read/Write translate
	// to io.EOF.  This unblocks the goroutine without a kill or signal.
	if fd >= 0 {
		_ = syscall.Shutdown(fd, syscall.SHUT_RDWR)
	}

	c.wg.Wait() // wait for any CGo call in flight to return

	c.mu.Lock()
	if c.epfd >= 0 {
		syscall.Close(c.epfd)
		c.epfd = -1
	}
	if c.ptr != nil {
		C.wolfssl_gtw_conn_close(c.ptr)
		c.ptr = nil
	}
	c.fd = -1
	c.mu.Unlock()

	return nil
}

func (c *WolfSSLGtwConn) LocalAddr() net.Addr {
	if c.localAddr == nil {
		return &net.TCPAddr{}
	}
	return c.localAddr
}

func (c *WolfSSLGtwConn) RemoteAddr() net.Addr {
	if c.remoteAddr == nil {
		return &net.TCPAddr{}
	}
	return c.remoteAddr
}

// SetDeadline implements net.Conn — sets both read and write deadlines.
func (c *WolfSSLGtwConn) SetDeadline(t time.Time) error {
	if err := c.SetReadDeadline(t); err != nil {
		return err
	}
	return c.SetWriteDeadline(t)
}

// SetReadDeadline implements net.Conn.  Stores the deadline in the struct so
// waitEpoll() can check it on each 500 ms timeout, and also sets SO_RCVTIMEO
// on the raw fd so the kernel-level recv() also obeys the deadline.
func (c *WolfSSLGtwConn) SetReadDeadline(t time.Time) error {
	c.mu.Lock()
	c.readDeadline = t
	c.mu.Unlock()
	return c.setSocketDeadline(syscall.SO_RCVTIMEO, t)
}

// SetWriteDeadline implements net.Conn — same as SetReadDeadline for writes.
func (c *WolfSSLGtwConn) SetWriteDeadline(t time.Time) error {
	c.mu.Lock()
	c.writeDeadline = t
	c.mu.Unlock()
	return c.setSocketDeadline(syscall.SO_SNDTIMEO, t)
}

// setSocketDeadline applies the deadline to the underlying TCP socket via
// SO_RCVTIMEO or SO_SNDTIMEO.  A zero time.Time clears the timeout.
func (c *WolfSSLGtwConn) setSocketDeadline(opt int, t time.Time) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed || c.fd < 0 {
		return net.ErrClosed
	}
	tv := syscall.NsecToTimeval(0)
	if !t.IsZero() {
		timeout := time.Until(t)
		if timeout < 0 {
			timeout = 0
		}
		tv = syscall.NsecToTimeval(timeout.Nanoseconds())
	}
	return syscall.SetsockoptTimeval(c.fd, syscall.SOL_SOCKET, opt, &tv)
}

// RawFD returns the underlying TCP file descriptor (for diagnostics).
func (c *WolfSSLGtwConn) RawFD() int { return c.fd }

// Pending returns the number of bytes buffered inside wolfSSL that have been
// decrypted but not yet consumed by the application.  Used by the http.Server
// to decide whether to drain before blocking on epoll.
func (c *WolfSSLGtwConn) Pending() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed || c.ptr == nil {
		return 0
	}
	return int(C.wolfssl_gtw_conn_pending(c.ptr))
}

// ── Address helpers ───────────────────────────────────────────────────────────

// gtwSocketAddr resolves the local or remote address of an fd via getsockname/getpeername.
func gtwSocketAddr(fd int, peer bool) net.Addr {
	if fd < 0 {
		return &net.TCPAddr{}
	}
	var (
		sa  syscall.Sockaddr
		err error
	)
	if peer {
		sa, err = syscall.Getpeername(fd)
	} else {
		sa, err = syscall.Getsockname(fd)
	}
	if err != nil {
		return &net.TCPAddr{}
	}
	switch a := sa.(type) {
	case *syscall.SockaddrInet4:
		ip := make(net.IP, net.IPv4len)
		copy(ip, a.Addr[:])
		return &net.TCPAddr{IP: ip, Port: a.Port}
	case *syscall.SockaddrInet6:
		ip := make(net.IP, net.IPv6len)
		copy(ip, a.Addr[:])
		return &net.TCPAddr{IP: ip, Port: a.Port}
	}
	return &net.TCPAddr{}
}
