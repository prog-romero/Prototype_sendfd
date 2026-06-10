//go:build linux && cgo

/*
 * tls_vanilla.go — Go CGo wrapper for vanilla HTTPS (wolfSSL only).
 *
 * Implements net.Conn using wolfssl_vanilla_conn_t from tls_vanilla.c.
 * NO libtlspeek. NO keylog callback. NO TLS session export.
 *
 * Used exclusively by RunVanillaHTTPS in loop_https.go.
 */
package httpmigrate

/*
#cgo CFLAGS:  -I/usr/local/include
#cgo LDFLAGS: -L/usr/local/lib -lwolfssl -lpthread

#include <stdlib.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include "tls_vanilla.h"
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

// ── WolfSSLVanillaCtx ────────────────────────────────────────────────────────

// WolfSSLVanillaCtx is a shared TLS 1.3 server context (one per gateway process).
type WolfSSLVanillaCtx struct {
	ptr *C.wolfssl_vanilla_ctx_t
}

// NewWolfSSLVanillaCtx creates a vanilla TLS context from PEM cert/key files.
func NewWolfSSLVanillaCtx(certFile, keyFile string) (*WolfSSLVanillaCtx, error) {
	ccert := C.CString(certFile)
	ckey := C.CString(keyFile)
	defer C.free(unsafe.Pointer(ccert))
	defer C.free(unsafe.Pointer(ckey))

	p := C.wolfssl_vanilla_ctx_new(ccert, ckey)
	if p == nil {
		return nil, fmt.Errorf("[tls_vanilla] wolfssl_vanilla_ctx_new failed (cert=%s key=%s)", certFile, keyFile)
	}
	return &WolfSSLVanillaCtx{ptr: p}, nil
}

// Free releases the wolfSSL context.
func (ctx *WolfSSLVanillaCtx) Free() {
	if ctx.ptr != nil {
		C.wolfssl_vanilla_ctx_free(ctx.ptr)
		ctx.ptr = nil
	}
}

// DoHandshake performs the TLS 1.3 server handshake on a raw TCP fd.
// Returns a WolfSSLVanillaConn ready for Read/Write.
func (ctx *WolfSSLVanillaCtx) DoHandshake(fd int) (*WolfSSLVanillaConn, error) {
	p := C.wolfssl_vanilla_handshake(ctx.ptr, C.int(fd))
	if p == nil {
		return nil, fmt.Errorf("[tls_vanilla] handshake failed fd=%d", fd)
	}
	return &WolfSSLVanillaConn{ptr: p, fd: fd}, nil
}

// ── WolfSSLVanillaConn (implements net.Conn) ──────────────────────────────────

// WolfSSLVanillaConn wraps a wolfSSL connection as a net.Conn.
// Read  → wolfssl_vanilla_read  → wolfSSL_read  (returns decrypted plaintext)
// Write → wolfssl_vanilla_write → wolfSSL_write (sends encrypted ciphertext)
type WolfSSLVanillaConn struct {
	mu         sync.Mutex
	wg         sync.WaitGroup
	ptr        *C.wolfssl_vanilla_conn_t
	fd         int
	closed     bool
	localAddr  net.Addr
	remoteAddr net.Addr
}

func waitEpoll(fd int, write bool) {
	if fd < 0 {
		return
	}
	epfd, err := syscall.EpollCreate1(0)
	if err != nil {
		time.Sleep(1 * time.Millisecond)
		return
	}
	defer syscall.Close(epfd)

	var ev syscall.EpollEvent
	ev.Fd = int32(fd)
	if write {
		ev.Events = syscall.EPOLLOUT | syscall.EPOLLRDHUP | syscall.EPOLLERR
	} else {
		ev.Events = syscall.EPOLLIN | syscall.EPOLLRDHUP | syscall.EPOLLERR
	}

	if err := syscall.EpollCtl(epfd, syscall.EPOLL_CTL_ADD, fd, &ev); err != nil {
		time.Sleep(1 * time.Millisecond)
		return
	}

	events := make([]syscall.EpollEvent, 1)
	for {
		n, err := syscall.EpollWait(epfd, events, -1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			break
		}
		if n > 0 {
			break
		}
	}
}

func (c *WolfSSLVanillaConn) Read(p []byte) (int, error) {
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

		n := C.wolfssl_vanilla_read(ptr, unsafe.Pointer(&p[0]), C.int(len(p)))
		
		c.mu.Lock()
		code := int(C.wolfssl_vanilla_get_error(ptr, n))
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
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SSL_ERROR_SYSCALL):
			return 0, io.EOF // clean or abrupt connection close
		default:
			return 0, fmt.Errorf("wolfssl_vanilla_read error code=%d", code)
		}
	}
}

func (c *WolfSSLVanillaConn) Write(p []byte) (int, error) {
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

		n := C.wolfssl_vanilla_write(ptr,
			unsafe.Pointer(&p[total]), C.int(len(p)-total))
		
		c.mu.Lock()
		code := int(C.wolfssl_vanilla_get_error(ptr, n))
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
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SSL_ERROR_SYSCALL):
			return total, io.EOF
		default:
			return total, fmt.Errorf("wolfssl_vanilla_write error code=%d", code)
		}
	}
	return total, nil
}

func (c *WolfSSLVanillaConn) Close() error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return nil
	}
	c.closed = true
	fd := c.fd
	c.mu.Unlock()

	// 1. Wake up any blocked C.wolfssl_vanilla_read/write calls
	if fd >= 0 {
		_ = syscall.Shutdown(fd, syscall.SHUT_RDWR)
	}

	// 2. Wait for all C calls to exit to prevent use-after-free
	c.wg.Wait()

	// 3. Safely free the C pointer
	c.mu.Lock()
	if c.ptr != nil {
		C.wolfssl_vanilla_close(c.ptr)
		c.ptr = nil
	}
	c.fd = -1
	c.mu.Unlock()

	return nil
}

func (c *WolfSSLVanillaConn) LocalAddr() net.Addr {
	if c.localAddr != nil {
		return c.localAddr
	}
	return &net.TCPAddr{}
}

func (c *WolfSSLVanillaConn) RemoteAddr() net.Addr {
	if c.remoteAddr != nil {
		return c.remoteAddr
	}
	return &net.TCPAddr{}
}

func (c *WolfSSLVanillaConn) SetDeadline(t time.Time) error      { return nil }
func (c *WolfSSLVanillaConn) SetReadDeadline(t time.Time) error  { return nil }
func (c *WolfSSLVanillaConn) SetWriteDeadline(t time.Time) error { return nil }

