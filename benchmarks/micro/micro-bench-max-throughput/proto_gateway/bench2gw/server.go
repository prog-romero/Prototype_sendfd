package bench2gw

/*
#include "bench2gw.h"
#include <stdlib.h>
*/
import "C"

import (
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

const targetSize = 128
const relayPayloadSize = int(C.sizeof_bench2_keepalive_payload_t)

type relayConn struct {
	fd       int
	clientFD int
	off      int
	payload  *C.bench2_keepalive_payload_t
}

func newRelayConn(fd int) (*relayConn, error) {
	payload := (*C.bench2_keepalive_payload_t)(
		C.malloc(C.size_t(C.sizeof_bench2_keepalive_payload_t)))
	if payload == nil {
		return nil, fmt.Errorf("malloc relay payload failed")
	}
	return &relayConn{fd: fd, clientFD: -1, payload: payload}, nil
}

func (r *relayConn) close() {
	if r == nil {
		return
	}
	if r.fd >= 0 {
		_ = syscall.Close(r.fd)
		r.fd = -1
	}
	if r.clientFD >= 0 {
		_ = syscall.Close(r.clientFD)
		r.clientFD = -1
	}
	if r.payload != nil {
		C.free(unsafe.Pointer(r.payload))
		r.payload = nil
	}
}

func (r *relayConn) recv() (bool, error) {
	if r == nil || r.fd < 0 || r.payload == nil {
		return false, fmt.Errorf("invalid relay connection")
	}
	if r.off >= relayPayloadSize {
		return r.clientFD >= 0, nil
	}

	buf := unsafe.Slice((*byte)(unsafe.Pointer(r.payload)), relayPayloadSize)
	oob := make([]byte, syscall.CmsgSpace(4))

	n, oobn, flags, _, err := syscall.Recvmsg(
		r.fd,
		buf[r.off:],
		oob,
		syscall.MSG_DONTWAIT,
	)
	if err != nil {
		if err == syscall.EAGAIN || err == syscall.EWOULDBLOCK {
			return false, nil
		}
		return false, err
	}
	if n == 0 || flags&syscall.MSG_CTRUNC != 0 {
		return false, fmt.Errorf("relay recvmsg closed/truncated")
	}

	if oobn > 0 {
		scms, err := syscall.ParseSocketControlMessage(oob[:oobn])
		if err != nil {
			return false, err
		}
		for _, scm := range scms {
			fds, err := syscall.ParseUnixRights(&scm)
			if err != nil {
				return false, err
			}
			for _, fd := range fds {
				if r.clientFD >= 0 {
					_ = syscall.Close(fd)
				} else {
					r.clientFD = fd
				}
			}
		}
	}

	r.off += n
	if r.off < relayPayloadSize {
		return false, nil
	}
	if r.clientFD < 0 {
		return false, fmt.Errorf("relay payload completed without fd")
	}
	return true, nil
}

// Run starts the bench2 prototype gateway listener on cfg.ListenAddr.
// It also starts the relay listener that receives forwarded sessions from
// wrong-owner containers.
func Run(cfg Config) error {
	if err := os.MkdirAll(cfg.SocketDir, 0o777); err != nil {
		log.Printf("[bench2gw] warning: mkdir %s: %v", cfg.SocketDir, err)
	}
	_ = os.Chmod(cfg.SocketDir, 0o777)

	/* TCP listener */
	ln, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", cfg.ListenAddr, err)
	}
	defer ln.Close()

	tcpLn, ok := ln.(*net.TCPListener)
	if !ok {
		return fmt.Errorf("unexpected listener type %T", ln)
	}
	f, err := tcpLn.File()
	if err != nil {
		return fmt.Errorf("listener file: %w", err)
	}
	defer f.Close()
	_ = tcpLn.Close()
	listenFD := int(f.Fd())
	if err := syscall.SetNonblock(listenFD, true); err != nil {
		return fmt.Errorf("set nonblock listen fd: %w", err)
	}

	cRelay := C.CString(cfg.RelaySocket)
	defer C.free(unsafe.Pointer(cRelay))
	relayFD := C.bench2gw_unix_server(cRelay)
	if relayFD < 0 {
		return fmt.Errorf("relay listen failed: %s", cfg.RelaySocket)
	}
	defer syscall.Close(int(relayFD))
	if err := syscall.SetNonblock(int(relayFD), true); err != nil {
		return fmt.Errorf("set nonblock relay fd: %w", err)
	}

	cCert := C.CString(cfg.CertFile)
	defer C.free(unsafe.Pointer(cCert))
	cKey := C.CString(cfg.KeyFile)
	defer C.free(unsafe.Pointer(cKey))

	ctx := C.bench2gw_new_ctx(cCert, cKey)
	if ctx == nil {
		return fmt.Errorf("bench2gw_new_ctx failed")
	}
	defer C.bench2gw_free_ctx(ctx)

	peekBuf := make([]byte, cfg.MaxPeekBytes)
	payload := (*C.bench2_keepalive_payload_t)(
		C.malloc(C.size_t(C.sizeof_bench2_keepalive_payload_t)))
	if payload == nil {
		return fmt.Errorf("malloc payload failed")
	}
	defer C.free(unsafe.Pointer(payload))

	epollFD, err := syscall.EpollCreate1(0)
	if err != nil {
		return fmt.Errorf("epoll create: %w", err)
	}
	defer syscall.Close(epollFD)

	if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, listenFD,
		&syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(listenFD)}); err != nil {
		return fmt.Errorf("epoll add listen fd: %w", err)
	}
	if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, int(relayFD),
		&syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(relayFD)}); err != nil {
		return fmt.Errorf("epoll add relay fd: %w", err)
	}

	log.Printf("[bench2gw] started listener=%s relay=%s [epoll]", cfg.ListenAddr, cfg.RelaySocket)

	events := make([]syscall.EpollEvent, 64)
	pending := make(map[int]*C.bench2gw_conn_t)
	relayConns := make(map[int]*relayConn)
	clientEvents := func(conn *C.bench2gw_conn_t) uint32 {
		events := uint32(syscall.EPOLLRDHUP)
		if C.bench2gw_conn_events(conn) == 2 {
			events |= syscall.EPOLLOUT
		} else {
			events |= syscall.EPOLLIN
		}
		return events
	}

	closePending := func(fd int) {
		if conn := pending[fd]; conn != nil {
			_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
			C.bench2gw_conn_free(conn)
			delete(pending, fd)
		}
	}
	closeRelayConn := func(fd int) {
		if conn := relayConns[fd]; conn != nil {
			_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
			conn.close()
			delete(relayConns, fd)
		}
	}

	for {
		n, err := syscall.EpollWait(epollFD, events, -1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return fmt.Errorf("epoll wait: %w", err)
		}

		for i := 0; i < n; i++ {
			fd := int(events[i].Fd)
			if fd == listenFD {
				for {
					conn := C.bench2gw_accept_start(ctx, C.int(listenFD))
					if conn == nil {
						break
					}

					clientFD := int(C.bench2gw_conn_fd(conn))
					if clientFD < 0 {
						C.bench2gw_conn_free(conn)
						continue
					}
					if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, clientFD,
						&syscall.EpollEvent{Events: clientEvents(conn), Fd: int32(clientFD)}); err != nil {
						C.bench2gw_conn_free(conn)
						log.Printf("[bench2gw] epoll add client fd=%d failed: %v", clientFD, err)
						continue
					}
					pending[clientFD] = conn
				}
				continue
			}

			if fd == int(relayFD) {
				for {
					connFD, _, err := syscall.Accept4(
						int(relayFD),
						syscall.SOCK_CLOEXEC|syscall.SOCK_NONBLOCK,
					)
					if err != nil {
						if err == syscall.EAGAIN || err == syscall.EWOULDBLOCK {
							break
						}
						log.Printf("[bench2gw] relay accept failed: %v", err)
						break
					}
					relayConn, err := newRelayConn(connFD)
					if err != nil {
						_ = syscall.Close(connFD)
						continue
					}
					if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, connFD,
						&syscall.EpollEvent{Events: syscall.EPOLLIN | syscall.EPOLLRDHUP, Fd: int32(connFD)}); err != nil {
						relayConn.close()
						log.Printf("[bench2gw] epoll add relay fd=%d failed: %v", connFD, err)
						continue
					}
					relayConns[connFD] = relayConn
				}
				continue
			}

			if relayConn := relayConns[fd]; relayConn != nil {
				if events[i].Events&syscall.EPOLLERR != 0 {
					closeRelayConn(fd)
					continue
				}

				if events[i].Events&syscall.EPOLLIN != 0 {
					done, err := relayConn.recv()
					if err != nil {
						log.Printf("[bench2gw] relay recv fd=%d failed: %v", fd, err)
						closeRelayConn(fd)
						continue
					}
					if !done {
						continue
					}

					clientFD := relayConn.clientFD
					relayPayload := relayConn.payload
					relayConn.clientFD = -1
					relayConn.payload = nil
					closeRelayConn(fd)

					fnName := payloadTarget(relayPayload)
					if fnName == "" {
						_ = syscall.Close(clientFD)
						C.free(unsafe.Pointer(relayPayload))
						continue
					}

					if err := dispatchToFunction(cfg.SocketDir, fnName, clientFD, relayPayload); err != nil {
						log.Printf("[bench2gw] relay dispatch failed fn=%s: %v", fnName, err)
					}
					C.free(unsafe.Pointer(relayPayload))
					continue
				}

				if events[i].Events&(syscall.EPOLLHUP|syscall.EPOLLRDHUP) != 0 {
					closeRelayConn(fd)
					continue
				}
			}

			conn := pending[fd]
			if conn == nil {
				_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
				_ = syscall.Close(fd)
				continue
			}

			if events[i].Events&(syscall.EPOLLERR|syscall.EPOLLHUP|syscall.EPOLLRDHUP) != 0 {
				closePending(fd)
				continue
			}

			var peekLen C.int
			rc := C.bench2gw_conn_step(
				conn,
				(*C.uchar)(unsafe.Pointer(&peekBuf[0])),
				C.size_t(len(peekBuf)),
				&peekLen,
				payload,
			)
			if rc == 0 {
				_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_MOD, fd,
					&syscall.EpollEvent{Events: clientEvents(conn), Fd: int32(fd)})
				continue
			}
			if rc < 0 {
				log.Printf("[bench2gw] conn step failed fd=%d", fd)
				closePending(fd)
				continue
			}

			_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
			delete(pending, fd)
			clientFD := int(C.bench2gw_conn_take_fd(conn))
			C.bench2gw_conn_free(conn)
			if clientFD < 0 {
				continue
			}

			fnName := payloadTarget(payload)
			if fnName == "" {
				parsedFn, ok := parseFunctionName(peekBuf[:int(peekLen)])
				if !ok {
					_ = syscall.Close(clientFD)
					continue
				}
				fnName = parsedFn
				setPayloadTarget(payload, fnName)
			}

			if err := dispatchToFunction(cfg.SocketDir, fnName, clientFD, payload); err != nil {
				log.Printf("[bench2gw] dispatch failed fn=%s: %v", fnName, err)
			}
		}
	}
}

func dispatchToFunction(socketDir, fnName string, clientFD int,
	payload *C.bench2_keepalive_payload_t) error {

	sockPath := filepath.Join(socketDir, fnName+".sock")
	cSock := C.CString(sockPath)
	defer C.free(unsafe.Pointer(cSock))

	rc := C.bench2gw_send_fd(
		cSock,
		C.int(clientFD),
		payload,
		C.size_t(C.sizeof_bench2_keepalive_payload_t),
	)
	if rc != 0 {
		return fmt.Errorf("bench2gw_send_fd failed sock=%s", sockPath)
	}
	return nil
}

func setPayloadTarget(payload *C.bench2_keepalive_payload_t, target string) {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	for i := range buf {
		buf[i] = 0
	}
	copy(buf, target)
}

func payloadTarget(payload *C.bench2_keepalive_payload_t) string {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	end := 0
	for end < len(buf) && buf[end] != 0 {
		end++
	}
	return string(buf[:end])
}
