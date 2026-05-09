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
// It also spawns runRelay() to receive FDs returned by wrong-owner workers.
func RunProto(cfg ProtoConfig) error {
	os.MkdirAll(cfg.SocketDir, 0777)

	// Start the relay listener in background
	go func() {
		if err := runRelay(cfg); err != nil {
			log.Printf("[benchhttp-ka] relay stopped: %v", err)
		}
	}()

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

	headerBuf := make([]byte, 8192)
	payload := (*C.httpmigrate_ka_payload_t)(C.malloc(C.size_t(C.sizeof_httpmigrate_ka_payload_t)))
	defer C.free(unsafe.Pointer(payload))

	log.Printf("[benchhttp-ka] started keep-alive zero-copy HTTP gateway on %s", cfg.ListenAddr)

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
			continue
		}

		fnName, ok := parseFunctionName(headerBuf[:int(hdrLen)])
		if !ok {
			log.Printf("[benchhttp-ka] failed to parse function name")
			syscall.Close(int(clientFD))
			continue
		}

		setPayloadTarget(payload, fnName)

		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[benchhttp-ka] dispatch failed fn=%s: %v", fnName, err)
		}
	}
}

// runRelay listens on cfg.RelaySocket for sessions forwarded back by wrong-owner
// workers. The payload already holds top1_rdtsc/cntfrq/top1_set and target_function.
func runRelay(cfg ProtoConfig) error {
	cRelay := C.CString(cfg.RelaySocket)
	defer C.free(unsafe.Pointer(cRelay))

	listenFD := C.httpmigrate_ka_unix_server(cRelay)
	if listenFD < 0 {
		return fmt.Errorf("relay listen failed: %s", cfg.RelaySocket)
	}
	defer syscall.Close(int(listenFD))

	payload := (*C.httpmigrate_ka_payload_t)(C.malloc(C.size_t(C.sizeof_httpmigrate_ka_payload_t)))
	defer C.free(unsafe.Pointer(payload))

	log.Printf("[benchhttp-ka] relay listener started on %s", cfg.RelaySocket)

	for {
		clientFD := C.httpmigrate_ka_accept_recv(listenFD, payload)
		if clientFD < 0 {
			continue
		}

		fnName := payloadTarget(payload)
		if fnName == "" {
			syscall.Close(int(clientFD))
			continue
		}

		log.Printf("[benchhttp-ka] relay: routing fd to fn=%s", fnName)
		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[benchhttp-ka] relay dispatch failed fn=%s: %v", fnName, err)
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
