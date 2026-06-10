//go:build linux && cgo

/*
 * tls_listener.go — Go CGo wrapper for the wolfSSL gateway bridge.
 *
 * Provides two types:
 *
 *   WolfSSLGtwCtx  — shared wolfSSL context (WOLFSSL_CTX*), created once at startup.
 *   WolfSSLGtwConn — per-connection handle that implements net.Conn.
 *                    When pushed to ChanListener, the existing OpenFaaS http.Server
 *                    reads decrypted (plaintext) HTTP via wolfSSL_read and writes
 *                    encrypted HTTPS responses via wolfSSL_write — transparently.
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
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// ── WolfSSLGtwCtx ────────────────────────────────────────────────────────────

// WolfSSLGtwCtx holds the shared wolfSSL context used for all HTTPS connections.
// One instance is created at gateway startup and reused for every connection.
type WolfSSLGtwCtx struct {
	ptr *C.wolfssl_gtw_ctx_t
}

// NewWolfSSLGtwCtx creates a wolfSSL context loading the given certificate and key.
// certFile/keyFile are PEM (standard TLS certificate format) file paths.
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

// Free releases the wolfSSL context.
func (c *WolfSSLGtwCtx) Free() {
	if c == nil || c.ptr == nil {
		return
	}
	C.wolfssl_gtw_ctx_free(c.ptr)
	c.ptr = nil
}

// SerialSize returns the byte size of a serialised (packed) TLS session state.
// Used to allocate the right buffer for tlsgw_peek_and_export.
func (c *WolfSSLGtwCtx) SerialSize() int {
	return int(C.TLSGW_SERIAL_SIZE)
}

// ── WolfSSLGtwConn ────────────────────────────────────────────────────────────

// WolfSSLGtwConn is a per-connection handle that wraps a wolfSSL session.
// It implements net.Conn so it can be pushed to ChanListener and handled by
// the existing OpenFaaS http.Server without any modification.
type WolfSSLGtwConn struct {
	ptr        *C.wolfssl_gtw_conn_t
	fd         int
	mu         sync.Mutex
	wg         sync.WaitGroup
	closed     bool
	localAddr  net.Addr
	remoteAddr net.Addr
}

// DoHandshake performs the TLS 1.3 handshake on an already-accepted raw TCP fd.
// Called inside a goroutine (one per connection) for parallel handshakes.
func (c *WolfSSLGtwCtx) DoHandshake(tcpFD int) (*WolfSSLGtwConn, error) {
	ptr := C.wolfssl_do_handshake(c.ptr, C.int(tcpFD))
	if ptr == nil {
		return nil, fmt.Errorf("[tls_listener] TLS handshake failed fd=%d", tcpFD)
	}
	fd := int(C.wolfssl_gtw_conn_fd(ptr))
	return &WolfSSLGtwConn{
		ptr:        ptr,
		fd:         fd,
		localAddr:  gtwSocketAddr(fd, false),
		remoteAddr: gtwSocketAddr(fd, true),
	}, nil
}

// PeekAndExport is used in prototype mode only.
// Returns (rawFD, fnName, serialBytes, top1Ns, err):
//   - rawFD >= 0: connection is to /function/<name>, wolfSSL detached, use sendfd.
//   - rawFD == -1: not a /function/ path, wolfSSL still active, push to ChanListener.
//   - rawFD == -2 or err != nil: fatal error, connection closed.
//
// skipTop1: set to true in SUM_PROD mode (no timing measurement needed).
func (conn *WolfSSLGtwConn) PeekAndExport(skipTop1 bool) (rawFD int, fnName string, serialBytes []byte, top1Ns uint64, err error) {
	serialBuf := make([]byte, int(C.TLSGW_SERIAL_SIZE))
	var cFnName [128]C.char
	var cTop1 C.uint64_t
	var cSerialSz C.int
	var cSkip C.int
	if skipTop1 {
		cSkip = 1
	}

	rc := C.tlsgw_peek_and_export(
		conn.ptr,
		&cFnName[0],
		128,
		&cTop1,
		unsafe.Pointer(&serialBuf[0]),
		&cSerialSz,
		cSkip,
	)

	switch {
	case rc >= 0:
		// Function path — wolfSSL freed internally by C, conn handle is invalid now.
		conn.mu.Lock()
		conn.ptr    = nil
		conn.closed = true
		conn.mu.Unlock()

		fn := C.GoString(&cFnName[0])
		return int(rc), fn, serialBuf[:int(cSerialSz)], uint64(cTop1), nil

	case rc == -1:
		// Not a /function/ path — wolfSSL still active on conn, use for ChanListener.
		return -1, "", nil, 0, nil

	default:
		// Fatal error — conn is already closed by C side.
		conn.mu.Lock()
		conn.ptr    = nil
		conn.closed = true
		conn.mu.Unlock()
		return -2, "", nil, 0, fmt.Errorf("[tls_listener] peek_and_export fatal error rc=%d", int(rc))
	}
}

// ── net.Conn interface ───────────────────────────────────────────────────────

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
			waitEpoll(c.fd, false)
			continue
		case int(C.SSL_ERROR_WANT_WRITE):
			waitEpoll(c.fd, true)
			continue
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E), int(C.SSL_ERROR_SYSCALL):
			return 0, io.EOF
		default:
			return 0, fmt.Errorf("wolfSSL_read error code=%d", code)
		}
	}
}

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
			waitEpoll(c.fd, false)
			continue
		case int(C.SSL_ERROR_WANT_WRITE):
			waitEpoll(c.fd, true)
			continue
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E), int(C.SSL_ERROR_SYSCALL):
			return total, io.EOF
		default:
			return total, fmt.Errorf("wolfSSL_write error code=%d", code)
		}
	}
	return total, nil
}

func (c *WolfSSLGtwConn) Close() error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return nil
	}
	c.closed = true
	fd := c.fd
	c.mu.Unlock()

	// 1. Wake up blocked C.wolfssl_read/write calls
	if fd >= 0 {
		_ = syscall.Shutdown(fd, syscall.SHUT_RDWR)
	}

	// 2. Wait for C calls to exit
	c.wg.Wait()

	// 3. Safely free
	c.mu.Lock()
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

func (c *WolfSSLGtwConn) SetDeadline(t time.Time) error {
	if err := c.SetReadDeadline(t); err != nil {
		return err
	}
	return c.SetWriteDeadline(t)
}

func (c *WolfSSLGtwConn) SetReadDeadline(t time.Time) error {
	return c.setSocketDeadline(syscall.SO_RCVTIMEO, t)
}

func (c *WolfSSLGtwConn) SetWriteDeadline(t time.Time) error {
	return c.setSocketDeadline(syscall.SO_SNDTIMEO, t)
}

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

// RawFD exposes the underlying TCP file descriptor.
// Used by stampTop1 timing logic to detect when data is ready.
func (c *WolfSSLGtwConn) RawFD() int { return c.fd }

// Pending returns the number of bytes already decrypted and buffered by wolfSSL.
func (c *WolfSSLGtwConn) Pending() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed || c.ptr == nil {
		return 0
	}
	return int(C.wolfssl_gtw_conn_pending(c.ptr))
}

// ── Address helpers ─────────────────────────────────────────────────────────

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
