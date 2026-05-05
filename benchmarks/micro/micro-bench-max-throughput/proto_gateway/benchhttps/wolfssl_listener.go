// wolfssl_listener.go
// CGO wrapper for wolfSSL-based TLS listener for vanilla gateway (no libtlspeek)
package benchhttps

/*
#cgo CFLAGS: -I/usr/local/include
#cgo LDFLAGS: -L/usr/local/lib -lwolfssl -lpthread
#include <stdlib.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include "wolfssl_listener.h"
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

type WolfSSLListener struct {
	ptr  *C.wolfssl_listener_t
	addr net.Addr
}

type WolfSSLConn struct {
	ptr        *C.wolfssl_conn_t
	fd         int
	localAddr  net.Addr
	remoteAddr net.Addr
	mu         sync.Mutex
	closed     bool
}

func NewWolfSSLListener(addr, certFile, keyFile string) (*WolfSSLListener, error) {
	cAddr := C.CString(addr)
	cCert := C.CString(certFile)
	cKey := C.CString(keyFile)
	defer C.free(unsafe.Pointer(cAddr))
	defer C.free(unsafe.Pointer(cCert))
	defer C.free(unsafe.Pointer(cKey))

	ptr := C.wolfssl_listener_new(cAddr, cCert, cKey)
	if ptr == nil {
		return nil, fmt.Errorf("wolfSSL listener init failed for %s", addr)
	}

	listener := &WolfSSLListener{ptr: ptr}
	if fd, err := listener.fd(); err == nil {
		listener.addr = socketAddr(fd, false)
	} else {
		listener.addr = &net.TCPAddr{}
	}

	return listener, nil
}

func (l *WolfSSLListener) Accept() (net.Conn, error) {
	connPtr := C.wolfssl_listener_accept(l.ptr)
	if connPtr == nil {
		return nil, fmt.Errorf("wolfSSL accept failed")
	}

	fd := int(C.wolfssl_conn_fd(connPtr))
	return &WolfSSLConn{
		ptr:        connPtr,
		fd:         fd,
		localAddr:  socketAddr(fd, false),
		remoteAddr: socketAddr(fd, true),
	}, nil
}

func (l *WolfSSLListener) fd() (int, error) {
	if l == nil || l.ptr == nil {
		return -1, net.ErrClosed
	}

	fd := int(C.wolfssl_listener_fd(l.ptr))
	if fd < 0 {
		return -1, fmt.Errorf("wolfSSL listener fd unavailable")
	}
	return fd, nil
}

func (l *WolfSSLListener) Close() error {
	if l == nil || l.ptr == nil {
		return nil
	}
	C.wolfssl_listener_close(l.ptr)
	l.ptr = nil
	return nil
}

func (l *WolfSSLListener) Addr() net.Addr {
	if l == nil || l.addr == nil {
		return &net.TCPAddr{}
	}
	return l.addr
}

func (c *WolfSSLConn) RawFD() int {
	return c.fd
}

func (c *WolfSSLConn) Pending() int {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.closed || c.ptr == nil {
		return 0
	}

	return int(C.wolfssl_conn_pending(c.ptr))
}

func (c *WolfSSLConn) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}

	for {
		c.mu.Lock()
		if c.closed || c.ptr == nil {
			c.mu.Unlock()
			return 0, net.ErrClosed
		}
		n := C.wolfssl_conn_read(c.ptr, unsafe.Pointer(&p[0]), C.int(len(p)))
		if n > 0 {
			c.mu.Unlock()
			return int(n), nil
		}
		code := int(C.wolfssl_conn_get_error(c.ptr, n))
		c.mu.Unlock()

		switch code {
		case int(C.SSL_ERROR_WANT_READ), int(C.SSL_ERROR_WANT_WRITE):
			continue
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E):
			return 0, io.EOF
		default:
			return 0, fmt.Errorf("wolfSSL_read failed: %d", code)
		}
	}
}

func (c *WolfSSLConn) Write(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}

	for {
		c.mu.Lock()
		if c.closed || c.ptr == nil {
			c.mu.Unlock()
			return 0, net.ErrClosed
		}
		n := C.wolfssl_conn_write(c.ptr, unsafe.Pointer(&p[0]), C.int(len(p)))
		if n > 0 {
			c.mu.Unlock()
			return int(n), nil
		}
		code := int(C.wolfssl_conn_get_error(c.ptr, n))
		c.mu.Unlock()

		switch code {
		case int(C.SSL_ERROR_WANT_READ), int(C.SSL_ERROR_WANT_WRITE):
			continue
		case int(C.SSL_ERROR_ZERO_RETURN), int(C.SOCKET_PEER_CLOSED_E):
			return 0, io.EOF
		default:
			return 0, fmt.Errorf("wolfSSL_write failed: %d", code)
		}
	}
}

func (c *WolfSSLConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.closed || c.ptr == nil {
		return nil
	}

	C.wolfssl_conn_close(c.ptr)
	c.ptr = nil
	c.closed = true
	c.fd = -1
	return nil
}

func (c *WolfSSLConn) LocalAddr() net.Addr {
	if c.localAddr == nil {
		return &net.TCPAddr{}
	}
	return c.localAddr
}

func (c *WolfSSLConn) RemoteAddr() net.Addr {
	if c.remoteAddr == nil {
		return &net.TCPAddr{}
	}
	return c.remoteAddr
}

func (c *WolfSSLConn) SetDeadline(t time.Time) error {
	if err := c.SetReadDeadline(t); err != nil {
		return err
	}
	return c.SetWriteDeadline(t)
}

func (c *WolfSSLConn) SetReadDeadline(t time.Time) error {
	return c.setSocketDeadline(syscall.SO_RCVTIMEO, t)
}

func (c *WolfSSLConn) SetWriteDeadline(t time.Time) error {
	return c.setSocketDeadline(syscall.SO_SNDTIMEO, t)
}

func (c *WolfSSLConn) setSocketDeadline(opt int, t time.Time) error {
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

func socketAddr(fd int, peer bool) net.Addr {
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
		return &net.TCPAddr{IP: ip, Port: a.Port, Zone: zoneName(a.ZoneId)}
	default:
		return &net.TCPAddr{}
	}
}

func zoneName(zoneID uint32) string {
	if zoneID == 0 {
		return ""
	}

	if iface, err := net.InterfaceByIndex(int(zoneID)); err == nil {
		return iface.Name
	}

	return ""
}
