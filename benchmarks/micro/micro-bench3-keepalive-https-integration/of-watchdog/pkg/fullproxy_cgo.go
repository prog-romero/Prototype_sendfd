//go:build linux && wolfssl

// Package pkg — fullproxy_cgo.go
//
// CGO glue for the full-proxy path. It links the wolfSSL + libtlspeek C bridge
// (wd_bridge.c) that performs all connection plumbing, and exposes the Go
// callback (goInvokeHandler) the bridge uses to run business logic.
//
// Build with: CGO_ENABLED=1 go build -tags wolfssl
// Requires wolfSSL and libtlspeek installed under /usr/local (see Dockerfile).

package pkg

/*
#cgo CFLAGS: -I/usr/local/include -DHAVE_SECRET_CALLBACK -DWOLFSSL_KEYLOG_EXPORT
#cgo LDFLAGS: -L/usr/local/lib -ltlspeek -lwolfssl -lpthread -lm
#include <stdlib.h>
#include <string.h>
#include "wd_bridge.h"
*/
import "C"

import (
	"fmt"
	"net/http"
	"path/filepath"
	"runtime/cgo"
	"unsafe"

	"github.com/openfaas/of-watchdog/config"
)

// fpConn carries the per-connection Go state across the C bridge via a
// runtime/cgo.Handle (we cannot pass a Go pointer holding pointers to C).
type fpConn struct {
	handler http.Handler
}

// initFullProxy creates the wolfSSL context once. With no cert/key the bridge
// still serves HTTP; HTTPS connections are then rejected.
func initFullProxy(cfg config.WatchdogConfig) error {
	cCert := C.CString(cfg.TLSCertFile)
	cKey := C.CString(cfg.TLSKeyFile)
	defer C.free(unsafe.Pointer(cCert))
	defer C.free(unsafe.Pointer(cKey))

	if rc := C.wd_bridge_init(cCert, cKey); rc != 0 {
		return fmt.Errorf("wd_bridge_init failed (cert=%q key=%q)", cfg.TLSCertFile, cfg.TLSKeyFile)
	}
	return nil
}

// serveFullProxyConn drives one migrated connection to completion. It takes
// ownership of fd1 (client) and fd2 (pipe); the C bridge closes them.
func serveFullProxyConn(fd1, fd2 int, payload []byte, cfg config.WatchdogConfig, handler http.Handler) {
	st := &fpConn{handler: handler}
	h := cgo.NewHandle(st)
	defer h.Delete()

	var pPtr *C.uchar
	if len(payload) > 0 {
		pPtr = (*C.uchar)(unsafe.Pointer(&payload[0]))
	}

	cOwn := C.CString(cfg.OwnFunctionName)
	cRelay := C.CString(filepath.Join(cfg.SendFDSocketDir, "provider.sock"))
	defer C.free(unsafe.Pointer(cOwn))
	defer C.free(unsafe.Pointer(cRelay))

	// Blocks for the whole keep-alive lifetime of the connection. The payload
	// slice is pinned by cgo for the duration of this call.
	C.wd_serve_conn(C.int(fd1), C.int(fd2),
		pPtr, C.int(len(payload)),
		cOwn, cRelay, C.uintptr_t(uintptr(h)))
}

//export goInvokeHandler
func goInvokeHandler(gohandle C.uintptr_t,
	req *C.uchar, reqLen C.int, shouldClose C.int,
	respOut **C.uchar, respLenOut *C.int) C.int {

	st, ok := cgo.Handle(uintptr(gohandle)).Value().(*fpConn)
	if !ok || st == nil {
		return 1
	}

	reqBytes := C.GoBytes(unsafe.Pointer(req), reqLen)
	resp := invokeBusinessLogic(st.handler, reqBytes, shouldClose != 0)
	if len(resp) == 0 {
		resp = errorResponseBytes(http.StatusInternalServerError, shouldClose != 0)
	}

	cbuf := C.malloc(C.size_t(len(resp)))
	if cbuf == nil {
		return 1
	}
	C.memcpy(cbuf, unsafe.Pointer(&resp[0]), C.size_t(len(resp)))
	*respOut = (*C.uchar)(cbuf)
	*respLenOut = C.int(len(resp))
	return 0
}
