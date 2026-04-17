package tlsmigratekeepalive

/*
#include "tlsmigratekeepalive.h"
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

func Run(cfg Config) error {
	if err := os.MkdirAll(cfg.SocketDir, 0o777); err != nil {
		log.Printf("[tlsmigrate-keepalive] warning: could not mkdir %s: %v", cfg.SocketDir, err)
	}
	_ = os.Chmod(cfg.SocketDir, 0o777)

	go func() {
		if err := runRelay(cfg); err != nil {
			log.Printf("[tlsmigrate-keepalive] relay stopped: %v", err)
		}
	}()

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

	cCert := C.CString(cfg.CertFile)
	defer C.free(unsafe.Pointer(cCert))
	cKey := C.CString(cfg.KeyFile)
	defer C.free(unsafe.Pointer(cKey))

	ctx := C.tlsmigratekeepalive_new_ctx(cCert, cKey)
	if ctx == nil {
		return fmt.Errorf("tlsmigratekeepalive_new_ctx failed")
	}
	defer C.tlsmigratekeepalive_free_ctx(ctx)

	headerBuf := make([]byte, cfg.MaxPeekBytes)
	payload := (*C.tlsmigrate_keepalive_payload_t)(C.malloc(C.size_t(C.sizeof_tlsmigrate_keepalive_payload_t)))
	if payload == nil {
		return fmt.Errorf("malloc payload failed")
	}
	defer C.free(unsafe.Pointer(payload))

	for {
		var hdrLen C.int
		clientFD := C.tlsmigratekeepalive_accept_peek_export(
			ctx,
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
			_ = syscall.Close(int(clientFD))
			continue
		}
		setPayloadTarget(payload, fnName)

		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[tlsmigrate-keepalive] initial dispatch failed fn=%s: %v", fnName, err)
		}
	}
}

func runRelay(cfg Config) error {
	cRelay := C.CString(cfg.RelaySocket)
	defer C.free(unsafe.Pointer(cRelay))

	listenFD := C.tlsmigratekeepalive_unix_server(cRelay)
	if listenFD < 0 {
		return fmt.Errorf("relay listen failed: %s", cfg.RelaySocket)
	}
	defer syscall.Close(int(listenFD))

	payload := (*C.tlsmigrate_keepalive_payload_t)(C.malloc(C.size_t(C.sizeof_tlsmigrate_keepalive_payload_t)))
	if payload == nil {
		return fmt.Errorf("malloc relay payload failed")
	}
	defer C.free(unsafe.Pointer(payload))

	for {
		clientFD := C.tlsmigratekeepalive_accept_recv(C.int(listenFD), payload)
		if clientFD < 0 {
			continue
		}

		fnName := payloadTarget(payload)
		if fnName == "" {
			_ = syscall.Close(int(clientFD))
			continue
		}

		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[tlsmigrate-keepalive] relay dispatch failed fn=%s: %v", fnName, err)
		}
	}
}

func dispatchToFunction(socketDir, fnName string, clientFD int, payload *C.tlsmigrate_keepalive_payload_t) error {
	sockPath := filepath.Join(socketDir, fnName+".sock")
	cSock := C.CString(sockPath)
	defer C.free(unsafe.Pointer(cSock))

	rc := C.tlsmigratekeepalive_send_fd(cSock, C.int(clientFD), payload, C.size_t(C.sizeof_tlsmigrate_keepalive_payload_t))
	if rc != 0 {
		return fmt.Errorf("sendfd failed sock=%s", sockPath)
	}
	return nil
}

func setPayloadTarget(payload *C.tlsmigrate_keepalive_payload_t, target string) {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	for i := range buf {
		buf[i] = 0
	}
	copy(buf, target)
}

func payloadTarget(payload *C.tlsmigrate_keepalive_payload_t) string {
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&payload.target_function[0])), targetSize)
	end := 0
	for end < len(buf) && buf[end] != 0 {
		end++
	}
	return string(buf[:end])
}