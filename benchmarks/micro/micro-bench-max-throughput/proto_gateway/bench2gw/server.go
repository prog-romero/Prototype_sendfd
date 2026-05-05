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

// Run starts the bench2 prototype gateway listener on cfg.ListenAddr.
// It also starts the relay listener that receives forwarded sessions from
// wrong-owner containers.
func Run(cfg Config) error {
	if err := os.MkdirAll(cfg.SocketDir, 0o777); err != nil {
		log.Printf("[bench2gw] warning: mkdir %s: %v", cfg.SocketDir, err)
	}
	_ = os.Chmod(cfg.SocketDir, 0o777)

	go func() {
		if err := runRelay(cfg); err != nil {
			log.Printf("[bench2gw] relay stopped: %v", err)
		}
	}()

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

	for {
		var peekLen C.int
		clientFD := C.bench2gw_accept_peek_export(
			ctx,
			C.int(listenFD),
			(*C.uchar)(unsafe.Pointer(&peekBuf[0])),
			C.size_t(len(peekBuf)),
			&peekLen,
			payload,
		)
		if clientFD < 0 {
			continue
		}

		fnName := payloadTarget(payload)
		if fnName == "" {
			parsedFn, ok := parseFunctionName(peekBuf[:int(peekLen)])
			if !ok {
				_ = syscall.Close(int(clientFD))
				continue
			}
			fnName = parsedFn
			setPayloadTarget(payload, fnName)
		}

		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[bench2gw] dispatch failed fn=%s: %v", fnName, err)
		}
	}
}

// runRelay listens on cfg.RelaySocket for sessions forwarded back by wrong-owner
// containers.  The payload already holds top1_rdtsc / cntfrq / top1_set from
// the container that peeked first.
func runRelay(cfg Config) error {
	cRelay := C.CString(cfg.RelaySocket)
	defer C.free(unsafe.Pointer(cRelay))

	listenFD := C.bench2gw_unix_server(cRelay)
	if listenFD < 0 {
		return fmt.Errorf("relay listen failed: %s", cfg.RelaySocket)
	}
	defer syscall.Close(int(listenFD))

	payload := (*C.bench2_keepalive_payload_t)(
		C.malloc(C.size_t(C.sizeof_bench2_keepalive_payload_t)))
	if payload == nil {
		return fmt.Errorf("malloc relay payload failed")
	}
	defer C.free(unsafe.Pointer(payload))

	for {
		clientFD := C.bench2gw_accept_recv(C.int(listenFD), payload)
		if clientFD < 0 {
			continue
		}

		fnName := payloadTarget(payload)
		if fnName == "" {
			_ = syscall.Close(int(clientFD))
			continue
		}

		if err := dispatchToFunction(cfg.SocketDir, fnName, int(clientFD), payload); err != nil {
			log.Printf("[bench2gw] relay dispatch failed fn=%s: %v", fnName, err)
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
