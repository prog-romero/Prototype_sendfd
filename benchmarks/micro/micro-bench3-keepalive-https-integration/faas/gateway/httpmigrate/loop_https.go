//go:build linux && cgo

// loop_https.go — HTTPS accept loops for vanilla and prototype modes.
//
// RunVanillaHTTPS: TLS termination (decrypt) → push WolfSSLGtwConn to ChanListener
//                 → existing OpenFaaS http.Server handles it as plain HTTP.
//                 The OpenFaaS router (r) is used exactly as in HTTP mode.
//
// RunLoopHTTPS: TLS handshake → tls_read_peek → if /function/*: sendfd path
//               (identical to HTTP RunLoop after the peek).
//               Non-function paths pushed to ChanListener → http.Server.
//
// Relay sockets are created lazily by EnsureRelaySocket (relay_server.go)
// when the first connection to each container is dispatched.

package httpmigrate

import (
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"syscall"
	"time"
)

// ── RunVanillaHTTPS ──────────────────────────────────────────────────────────

// RunVanillaHTTPS starts an HTTPS listener on tlsPort using ONLY wolfSSL.
//
// Architecture:
//   1. syscall.Accept  → raw TCP fd
//   2. goroutine       → wolfssl_vanilla_handshake (TLS 1.3, no callbacks)
//   3. WolfSSLVanillaConn pushed to ChanListener
//   4. http.Server reads decrypted HTTP via wolfSSL_read
//              writes encrypted HTTPS via wolfSSL_write
//   5. OpenFaaS router handles /function/* /system/* unchanged
//
// No libtlspeek. No keylog callback. No session export.
func RunVanillaHTTPS(
	tlsPort int,
	certFile, keyFile string,
	handler http.Handler,
	skipTop1 bool,
) error {
	ctx, err := NewWolfSSLVanillaCtx(certFile, keyFile)
	if err != nil {
		return fmt.Errorf("[vanilla-https] wolfSSL init: %w", err)
	}
	defer ctx.Free()

	listenFD, err := rawTCPListen(tlsPort)
	if err != nil {
		return fmt.Errorf("[vanilla-https] listen :%d: %w", tlsPort, err)
	}
	defer syscall.Close(listenFD)

	addr := &net.TCPAddr{Port: tlsPort}
	chanLis := NewChanListener(addr, 512)

	var srvHandler http.Handler
	if skipTop1 {
		srvHandler = handler
	} else {
		srvHandler = VanillaTimingMiddleware(handler)
	}

	s := &http.Server{
		Handler:        srvHandler,
		ReadTimeout:    60 * time.Second,
		WriteTimeout:   60 * time.Second,
		MaxHeaderBytes: 1 << 20,
	}
	if !skipTop1 {
		s.ConnState = StartVanillaTimingObserver()
	}

	go func() {
		if err := s.Serve(chanLis); err != nil && err != http.ErrServerClosed {
			log.Printf("[vanilla-https] HTTP server error: %v\n", err)
		}
	}()

	log.Printf("[vanilla-https] RunVanillaHTTPS listening on :%d (SUM_PROD=%v)\n",
		tlsPort, skipTop1)

	// ── Accept loop ──────────────────────────────────────────────────────────
	for {
		connFD, _, err := syscall.Accept(listenFD)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return fmt.Errorf("[vanilla-https] accept: %w", err)
		}

		go func(fd int) {
			conn, err := ctx.DoHandshake(fd)
			if err != nil {
				// DoHandshake already closed fd on failure.
				log.Printf("[vanilla-https] handshake fd=%d: %v\n", fd, err)
				return
			}
			if pushErr := chanLis.Push(conn); pushErr != nil {
				log.Printf("[vanilla-https] chanLis.Push failed: %v\n", pushErr)
				_ = conn.Close()
			}
		}(connFD)
	}
}


// ── RunLoopHTTPS ─────────────────────────────────────────────────────────────

// RunLoopHTTPS is the HTTPS equivalent of RunLoop (prototype mode).
//
// Workflow per connection:
//   1. syscall.Accept       → raw TCP fd
//   2. goroutine            → wolfSSL TLS handshake (keys captured)
//   3. wait for kernel data → top1_ns = getMonotonicNs()
//   4. tls_read_peek        → stateless decrypt, kernel buffer unchanged
//   5a. /function/<name>    → export TLS serial, detach wolfSSL, sendfd path
//   5b. other path          → WolfSSLGtwConn pushed to ChanListener → http.Server
//
// skipTop1: true in SUM_PROD mode (top1 not set in payload).
func RunLoopHTTPS(
	tlsPort int,
	certFile, keyFile string,
	handler http.Handler,
	providerURL string,
	notifier CompletionNotifier,
	skipTop1 bool,
) error {
	if err := os.MkdirAll(SocketDir, 0o777); err != nil {
		log.Printf("[httpmigrate-https] warning: mkdir %s: %v\n", SocketDir, err)
	}

	ctx, err := NewWolfSSLGtwCtx(certFile, keyFile)
	if err != nil {
		return fmt.Errorf("[httpmigrate-https] wolfSSL init: %w", err)
	}
	defer ctx.Free()

	listenFD, err := rawTCPListen(tlsPort)
	if err != nil {
		return fmt.Errorf("[httpmigrate-https] listen :%d: %w", tlsPort, err)
	}
	defer syscall.Close(listenFD)

	// Relay sockets are created lazily by EnsureRelaySocket inside
	// dispatchMigrateHTTPS when the first connection to each container is dispatched.
	// No explicit relay goroutine needed here.

	// ChanListener for non-function HTTPS paths (OpenFaaS admin, UI, etc.).
	addr := &net.TCPAddr{Port: tlsPort}
	chanLis := NewChanListener(addr, 512)

	s := &http.Server{
		Handler:        VanillaTimingMiddleware(handler),
		ConnState:      StartVanillaTimingObserver(),
		ReadTimeout:    60 * time.Second,
		WriteTimeout:   60 * time.Second,
		MaxHeaderBytes: 1 << 20,
	}
	go func() {
		if err := s.Serve(chanLis); err != nil && err != http.ErrServerClosed {
			log.Printf("[httpmigrate-https] HTTP server error: %v\n", err)
		}
	}()

	log.Printf("[httpmigrate-https] RunLoopHTTPS listening on :%d\n", tlsPort)

	// ── Accept loop ──────────────────────────────────────────────────────────
	for {
		connFD, _, err := syscall.Accept(listenFD)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return fmt.Errorf("[httpmigrate-https] accept: %w", err)
		}

		go handleConnHTTPS(ctx, connFD, chanLis, providerURL, notifier, skipTop1)
	}
}

// handleConnHTTPS handles one accepted HTTPS connection in a goroutine.
// This is the HTTPS equivalent of handleConn in loop.go.
func handleConnHTTPS(
	ctx *WolfSSLGtwCtx,
	connFD int,
	chanLis *ChanListener,
	providerURL string,
	notifier CompletionNotifier,
	skipTop1 bool,
) {
	// Step 1: TLS handshake (blocking inside the goroutine — does not block others).
	conn, err := ctx.DoHandshake(connFD)
	if err != nil {
		log.Printf("[handleConnHTTPS] handshake fd=%d: %v\n", connFD, err)
		return // DoHandshake already closed connFD on failure
	}

	// Step 2: Wait for data, stamp top1, tls_read_peek, parse function name.
	var rawFD int
	var fnName string
	var serialBytes []byte
	var top1Ns uint64
	for {
		conn.WaitEpoll(false)
		rawFD, fnName, serialBytes, top1Ns, err = conn.PeekAndExport(skipTop1)
		if err != nil {
			log.Printf("[handleConnHTTPS] PeekAndExport fd=%d: %v\n", connFD, err)
			return
		}
		if rawFD != 0 {
			break
		}
		// rawFD == 0 means EAGAIN or incomplete record, keep waiting
	}

	if rawFD >= 0 && fnName != "" {
		// ── Prototype sendfd path (same as HTTP mode after this point) ────────
		log.Printf("[handleConnHTTPS] MIGRATE path fn=%s rawFD=%d\n", fnName, rawFD)

		// Build the HTTPS payload (timing info + TLS serial bytes).
		var top1Val uint64
		if !skipTop1 {
			top1Val = top1Ns
		}
		payload := NewPayloadHTTPS(top1Val, fnName, serialBytes)

		if err := dispatchMigrateHTTPS(rawFD, fnName, payload, providerURL, notifier); err != nil {
			log.Printf("[handleConnHTTPS] dispatch FAILED fn=%s: %v\n", fnName, err)
			_ = syscall.Close(rawFD)
		}
		return
	}

	// ── Non-function HTTPS path: push to ChanListener → http.Server ──────────
	// The WolfSSLGtwConn.Read/Write transparently decrypts/encrypts for the router.
	log.Printf("[handleConnHTTPS] VANILLA HTTPS path connFD=%d\n", connFD)
	if pushErr := chanLis.Push(conn); pushErr != nil {
		log.Printf("[handleConnHTTPS] chanLis.Push failed: %v\n", pushErr)
		_ = conn.Close()
	}
}

// ── rawTCPListen ─────────────────────────────────────────────────────────────
// Shared TCP bind+listen helper for both HTTPS loops.
// Same logic as RunLoop in loop.go.
func rawTCPListen(port int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_INET6, syscall.SOCK_STREAM, 0)
	if err != nil {
		fd, err = syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
		if err != nil {
			return -1, fmt.Errorf("socket: %w", err)
		}
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		var sa4 syscall.SockaddrInet4
		sa4.Port = port
		if err := syscall.Bind(fd, &sa4); err != nil {
			_ = syscall.Close(fd)
			return -1, fmt.Errorf("bind IPv4 :%d: %w", port, err)
		}
	} else {
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_IPV6, syscall.IPV6_V6ONLY, 0)
		var sa6 syscall.SockaddrInet6
		sa6.Port = port
		if err := syscall.Bind(fd, &sa6); err != nil {
			_ = syscall.Close(fd)
			return -1, fmt.Errorf("bind IPv6 :%d: %w", port, err)
		}
	}
	if err := syscall.Listen(fd, 4096); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("listen :%d: %w", port, err)
	}
	return fd, nil
}
