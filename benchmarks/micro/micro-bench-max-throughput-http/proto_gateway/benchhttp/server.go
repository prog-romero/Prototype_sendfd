package benchhttp

/*
#cgo CFLAGS: -I${SRCDIR}
#cgo LDFLAGS: -lpthread

#include "httpmigrate_ka.h"
#include <stdlib.h>
*/
import "C"
import (
	"bytes"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

const targetSize = 128

// RunProto starts the keep-alive zero-copy HTTP gateway.
// It handles both direct TCP ingress and relay UDS ingress in one epoll loop.
func RunProto(cfg ProtoConfig) error {
	os.MkdirAll(cfg.SocketDir, 0777)

	addr, err := net.ResolveTCPAddr("tcp", cfg.ListenAddr)
	if err != nil {
		return err
	}

	listenFD, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	if err != nil {
		return err
	}
	defer syscall.Close(listenFD)

	syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)

	var sockaddr syscall.SockaddrInet4
	sockaddr.Port = addr.Port
	copy(sockaddr.Addr[:], addr.IP.To4())

	if err := syscall.Bind(listenFD, &sockaddr); err != nil {
		return err
	}
	if err := syscall.Listen(listenFD, 4096); err != nil {
		return err
	}
	if C.httpmigrate_ka_set_nonblocking(C.int(listenFD)) != 0 {
		return fmt.Errorf("set nonblock listen fd failed")
	}

	cRelay := C.CString(cfg.RelaySocket)
	defer C.free(unsafe.Pointer(cRelay))
	relayFD := int(C.httpmigrate_ka_unix_server(cRelay))
	if relayFD < 0 {
		return fmt.Errorf("relay listen failed: %s", cfg.RelaySocket)
	}
	defer syscall.Close(relayFD)

	headerBuf := make([]byte, 8192)
	payload := (*C.httpmigrate_ka_payload_t)(C.malloc(C.size_t(C.sizeof_httpmigrate_ka_payload_t)))
	defer C.free(unsafe.Pointer(payload))
	relayPayload := (*C.httpmigrate_ka_payload_t)(C.malloc(C.size_t(C.sizeof_httpmigrate_ka_payload_t)))
	defer C.free(unsafe.Pointer(relayPayload))

	epollFD, err := syscall.EpollCreate1(0)
	if err != nil {
		return fmt.Errorf("epoll create: %w", err)
	}
	defer syscall.Close(epollFD)

	if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, listenFD,
		&syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(listenFD)}); err != nil {
		return fmt.Errorf("epoll add listen fd: %w", err)
	}
	if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, relayFD,
		&syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(relayFD)}); err != nil {
		return fmt.Errorf("epoll add relay fd: %w", err)
	}

	log.Printf("[benchhttp-ka] started keep-alive zero-copy HTTP gateway on %s relay=%s [epoll]",
		cfg.ListenAddr, cfg.RelaySocket)

	events := make([]syscall.EpollEvent, 64)

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
					var hdrLen C.int
					clientFD := C.httpmigrate_ka_accept_peek(
						C.int(listenFD),
						(*C.uchar)(unsafe.Pointer(&headerBuf[0])),
						C.size_t(len(headerBuf)),
						&hdrLen,
						payload,
					)
					if clientFD < 0 {
						break
					}

					fnName, ok := parseFunctionName(headerBuf[:int(hdrLen)])
					if !ok {
						log.Printf("[benchhttp-ka] failed to parse function name")
						_ = syscall.Close(int(clientFD))
						continue
					}

					setPayloadTarget(payload, fnName)
					if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
						log.Printf("[benchhttp-ka] dispatch failed fn=%s: %v", fnName, err)
					}
				}
				continue
			}

			if fd == relayFD {
				for {
					clientFD := C.httpmigrate_ka_accept_recv(C.int(relayFD), relayPayload)
					if clientFD < 0 {
						break
					}

					fnName := payloadTarget(relayPayload)
					if fnName == "" {
						_ = syscall.Close(int(clientFD))
						continue
					}

					if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), relayPayload); err != nil {
						log.Printf("[benchhttp-ka] relay dispatch failed fn=%s: %v", fnName, err)
					}
				}
			}
		}
	}
}

func dispatchToFunction(socketDir, fnName string, clientFD int,
	payload *C.httpmigrate_ka_payload_t) error {

	sockPath := filepath.Join(socketDir, fnName+".sock")
	cSock := C.CString(sockPath)
	defer C.free(unsafe.Pointer(cSock))

	rc := C.httpmigrate_ka_send_fd(
		cSock,
		C.int(clientFD),
		payload,
		C.size_t(C.sizeof_httpmigrate_ka_payload_t),
	)
	if rc != 0 {
		return fmt.Errorf("httpmigrate_ka_send_fd failed sock=%s", sockPath)
	}
	return nil
}

func setPayloadTarget(payload *C.httpmigrate_ka_payload_t, target string) {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	for i := range buf {
		buf[i] = 0
	}
	copy(buf, target)
}

func payloadTarget(payload *C.httpmigrate_ka_payload_t) string {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	end := 0
	for end < len(buf) && buf[end] != 0 {
		end++
	}
	return string(buf[:end])
}

func parseFunctionName(peeked []byte) (string, bool) {
	lineEnd := bytes.Index(peeked, []byte("\r\n"))
	if lineEnd < 0 {
		lineEnd = bytes.IndexByte(peeked, '\n')
	}
	if lineEnd < 0 {
		lineEnd = len(peeked)
	}
	line := bytes.TrimSpace(peeked[:lineEnd])
	parts := bytes.SplitN(line, []byte(" "), 3)
	if len(parts) < 2 {
		return "", false
	}
	path := string(parts[1])
	if !strings.HasPrefix(path, "/function/") {
		return "", false
	}
	name := strings.TrimPrefix(path, "/function/")
	if idx := strings.IndexAny(name, "/? "); idx != -1 {
		name = name[:idx]
	}
	return name, name != ""
}
