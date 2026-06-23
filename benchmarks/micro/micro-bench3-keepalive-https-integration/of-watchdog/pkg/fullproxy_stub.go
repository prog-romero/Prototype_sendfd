//go:build linux && !wolfssl

// Package pkg — fullproxy_stub.go
//
// Stub used when the watchdog is built WITHOUT the wolfSSL toolchain
// (CGO_ENABLED=0, no -tags wolfssl). This keeps the legacy CGO-free build
// compiling. Full-proxy mode is unavailable in such a build: the existing
// relay-to-C-worker path (SendFDFullProxy=false) is unaffected.

package pkg

import (
	"fmt"
	"log"
	"net/http"
	"syscall"

	"github.com/openfaas/of-watchdog/config"
)

func initFullProxy(cfg config.WatchdogConfig) error {
	return fmt.Errorf("full-proxy mode requires a wolfSSL build (CGO_ENABLED=1, -tags wolfssl)")
}

func serveFullProxyConn(fd1, fd2 int, payload []byte, cfg config.WatchdogConfig, handler http.Handler) {
	log.Printf("[sendfd] full-proxy requested but this watchdog was built without wolfSSL support")
	if fd1 >= 0 {
		_ = syscall.Close(fd1)
	}
	if fd2 >= 0 {
		_ = syscall.Close(fd2)
	}
}
