//go:build linux

// Package httpmigrate — loop.go
//
// RunLoop replaces s.ListenAndServe() in main.go when HTTPMIGRATE_ENABLE=1.
// It owns the raw TCP socket on tcpPort and routes each accepted connection:
//
//   - /function/<name> requests  →  sendfd migration path: the raw fd is sent to
//     the faasd-provider (ProviderSock), which forwards it — with the function
//     name — to the function's watchdog; the watchdog then takes full ownership
//     of the TCP connection and replies directly to the client.
//   - Everything else             →  vanilla HTTP path via ChanListener,
//     served by the http.Server with the gorilla/mux router.

package httpmigrate

import (
	"bytes"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"strings"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

// getMonotonicNs returns monotonic nanoseconds using CLOCK_MONOTONIC_RAW.
// If CLOCK_MONOTONIC_RAW is unavailable, it falls back to CLOCK_MONOTONIC so
// gateway and function worker timestamps remain comparable.
func getMonotonicNs() uint64 {
	var ts unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC_RAW, &ts); err != nil {
		log.Printf("[httpmigrate] CLOCK_MONOTONIC_RAW unavailable: %v, falling back to CLOCK_MONOTONIC\n", err)
		if err2 := unix.ClockGettime(unix.CLOCK_MONOTONIC, &ts); err2 != nil {
			log.Printf("[httpmigrate] CLOCK_MONOTONIC unavailable: %v\n", err2)
			return uint64(time.Now().UnixNano())
		}
	}
	return uint64(ts.Sec)*1_000_000_000 + uint64(ts.Nsec)
}

// RunLoop opens a raw TCP socket on tcpPort, accepts connections, and
// routes them to either the sendfd path or the vanilla HTTP path.
//
// handler is typically the gorilla/mux router from main.go.
// providerURL is the URL of the faasd provider (e.g. "http://faasd:8081").
// notifier is called for Prometheus metrics when a function completes.
// scaleFn (may be nil) performs scale-from-zero for a migrated function before
// its fd is handed to the container, mirroring the vanilla router path.
//
// RunLoop blocks indefinitely.
func RunLoop(tcpPort int, handler http.Handler, providerURL string, notifier CompletionNotifier, scaleFn ScaleFunc) error {
	// Ensure the shared socket directory exists on the host (bind-mounted
	// into containers at /run/tlsmigrate).
	if err := os.MkdirAll(SocketDir, 0o777); err != nil {
		log.Printf("[httpmigrate] warning: mkdir %s: %v\n", SocketDir, err)
	}

	// ── Raw TCP listener ──────────────────────────────────────────────────
	listenFD, err := syscall.Socket(syscall.AF_INET6, syscall.SOCK_STREAM, 0)
	if err != nil {
		// Fallback to IPv4-only if IPv6 is not available.
		listenFD, err = syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
		if err != nil {
			return fmt.Errorf("socket: %w", err)
		}
		_ = syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		var sa4 syscall.SockaddrInet4
		sa4.Port = tcpPort
		if err := syscall.Bind(listenFD, &sa4); err != nil {
			_ = syscall.Close(listenFD)
			return fmt.Errorf("bind :%d: %w", tcpPort, err)
		}
	} else {
		_ = syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		// Disable IPV6_V6ONLY so the socket also accepts IPv4 (dual-stack).
		_ = syscall.SetsockoptInt(listenFD, syscall.IPPROTO_IPV6, syscall.IPV6_V6ONLY, 0)
		var sa6 syscall.SockaddrInet6
		sa6.Port = tcpPort
		if err := syscall.Bind(listenFD, &sa6); err != nil {
			_ = syscall.Close(listenFD)
			return fmt.Errorf("bind :%d (IPv6): %w", tcpPort, err)
		}
	}

	if err := syscall.Listen(listenFD, 4096); err != nil {
		_ = syscall.Close(listenFD)
		return fmt.Errorf("listen :%d: %w", tcpPort, err)
	}
	defer syscall.Close(listenFD)

	// ── ChanListener feeds the standard HTTP server ────────────────────────
	addr := &net.TCPAddr{Port: tcpPort}
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
			log.Printf("[httpmigrate] HTTP server error: %v\n", err)
		}
	}()

	log.Printf("[httpmigrate] RunLoop listening on :%d  (migrate path active)\n", tcpPort)

	// ── Accept loop ───────────────────────────────────────────────────────
	for {
		connFD, _, acceptErr := syscall.Accept(listenFD)
		if acceptErr != nil {
			if acceptErr == syscall.EINTR {
				continue
			}
			return fmt.Errorf("accept: %w", acceptErr)
		}
		go handleConn(connFD, chanLis, providerURL, notifier, scaleFn)
	}
}

// handleConn is spawned for each accepted raw fd.  It stamps top1_rdtsc
// BEFORE the MSG_PEEK call (matching the convention in the existing bench
// and the function worker's keepalive loop).
func handleConn(connFD int, chanLis *ChanListener, providerURL string, notifier CompletionNotifier, scaleFn ScaleFunc) {
	// ── Stamp top1 just before peeking ────────────────────────────────────
	top1Ns := getMonotonicNs()

	// ── MSG_PEEK to read the request line without consuming it ────────────
	var peekBuf [4096]byte
	n, _, peekErr := syscall.Recvfrom(connFD, peekBuf[:], syscall.MSG_PEEK)
	if peekErr != nil || n <= 0 {
		_ = syscall.Close(connFD)
		return
	}

	fnName := parseFunctionName(peekBuf[:n])

	if fnName != "" {
		// ── Scale-from-zero BEFORE migrating ──────────────────────────────
		// The migrate path bypasses the gorilla/mux router where the vanilla
		// scale-from-zero middleware lives, so we invoke the same scaler here
		// (over scaleFn) to bring a 0-replica function up before handing off its
		// fd. Only the connection's first request reaches this point; subsequent
		// keep-alive requests go straight to the container. Skipped if scaleFn is
		// nil (scale_from_zero disabled). The fd here is plaintext HTTP, so on
		// failure we can return a proper 503 to the client.
		if scaleFn != nil {
			if err := scaleFn(fnName); err != nil {
				log.Printf("[loop] scale-from-zero FAILED fn=%s connFD=%d: %v\n", fnName, connFD, err)
				writeHTTPError(connFD, http.StatusServiceUnavailable, "function unavailable: "+err.Error())
				_ = syscall.Close(connFD)
				return
			}
		}

		// ── Sendfd / migrate path ─────────────────────────────────────────
		// [MICROBENCH] top1 (gateway) = timestamp stamped just before the peek
		// above. It is logged DIRECTLY here; the watchdog logs top2 on its side,
		// and migration_ns = top2 - top1 is computed offline. top1 is no longer
		// used for the measurement — the payload still carries it only as the
		// wire "pre-routed" flag (top1_set), consumed by the watchdog for routing.
		if microbenchOn {
			log.Printf("[MICROBENCH] proto_top1_ns=%d\n", top1Ns)
		}
		payload := NewPayload(top1Ns, fnName)

		if err := dispatchMigrate(connFD, fnName, payload, providerURL, notifier); err != nil {
			log.Printf("[loop] dispatch FAILED fn=%s connFD=%d: %v\n", fnName, connFD, err)
			// connFD was NOT closed by dispatchMigrate on error — close it here.
			_ = syscall.Close(connFD)
		}
		// On success connFD is owned by the watchdog/worker (closed inside sendfd2WithState).
		return
	}

	// ── Vanilla HTTP path: wrap fd as net.Conn and push to ChanListener ───
	conn, wrapErr := wrapRawFD(connFD, peekBuf[:n])
	if wrapErr != nil {
		log.Printf("[httpmigrate] wrapRawFD: %v\n", wrapErr)
		_ = syscall.Close(connFD)
		return
	}
	if pushErr := chanLis.Push(conn); pushErr != nil {
		_ = conn.Close()
	}
}

// parseFunctionName extracts the function name from a peeked HTTP request line.
// It returns the name (e.g. "timing-fn-a") if the path starts with /function/,
// and an empty string for all other paths.
func parseFunctionName(peeked []byte) string {
	// Find the first line terminator.
	lineEnd := bytes.IndexByte(peeked, '\n')
	if lineEnd < 0 {
		lineEnd = len(peeked)
	}
	line := bytes.TrimSpace(peeked[:lineEnd])

	// "METHOD /path HTTP/1.x"
	parts := bytes.SplitN(line, []byte(" "), 3)
	if len(parts) < 2 {
		return ""
	}
	path := string(parts[1])

	const prefix = "/function/"
	if !strings.HasPrefix(path, prefix) {
		return ""
	}
	name := strings.TrimPrefix(path, prefix)
	if idx := strings.IndexAny(name, "/? "); idx != -1 {
		name = name[:idx]
	}
	return name
}

// wrapRawFD converts a raw syscall fd into a net.Conn and prepends the already-
// peeked bytes so that the HTTP server can parse the full request.
func wrapRawFD(fd int, peeked []byte) (net.Conn, error) {
	f := os.NewFile(uintptr(fd), "tcp-migrate-conn")
	if f == nil {
		return nil, fmt.Errorf("os.NewFile returned nil for fd=%d", fd)
	}
	inner, err := net.FileConn(f)
	_ = f.Close() // FileConn dup'd the fd; we no longer need the os.File wrapper.
	if err != nil {
		return nil, fmt.Errorf("net.FileConn: %w", err)
	}
	return NewConnRW(inner, peeked), nil
}
