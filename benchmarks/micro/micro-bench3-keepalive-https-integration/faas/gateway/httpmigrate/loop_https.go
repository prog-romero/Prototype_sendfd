//go:build linux && cgo

// loop_https.go — HTTPS accept loops for vanilla and prototype modes.
//
// ── ARCHITECTURE (epoll-driven, zero per-connection goroutines) ──────────────
//
// RunLoopHTTPS (prototype mode, HTTPMIGRATE_ENABLE=1 + HTTPS_ENABLE=1):
//   - numEpollWorkers (4) goroutines, each with its own epoll fd and listen fd.
//   - SO_REUSEPORT on all listen fds → kernel distributes connections evenly.
//   - listenFD    → accept() → wolfssl_accept_start() → add clientFD to epoll
//   - clientFD    → wolfssl_handshake_step() per event until done
//                → tlsgw_peek_and_export_nb():
//                    rc > 0  (/function/<name>): a goroutine sends the raw fd +
//                             exported TLS state to the faasd-provider over a
//                             Unix socket (ProviderSock). The provider resolves
//                             the target container and forwards the fd + TLS
//                             state + function name to the function's watchdog,
//                             which resumes the session and replies DIRECTLY to
//                             the client. The gateway keeps no fd for this conn.
//                    rc == -1 (other path, e.g. /system/*): push WolfSSLGtwConn
//                             to ChanListener so the local http.Server serves it.
//
// RunVanillaHTTPS (vanilla mode, HTTPMIGRATE_ENABLE=0 + HTTPS_ENABLE=1):
//   - Same 4-goroutine SO_REUSEPORT architecture.
//   - After handshake, conn is pushed to ChanListener immediately (no peek).
//   - wolfssl_accept_start_plain() is used: NO tlspeek keylog callback,
//     NO libtlspeek involvement at any point in the connection lifetime.
//
// KEY PROPERTIES:
//   - Zero goroutines created per connection → no goroutine leak.
//   - EpollWait(-1) blocks when idle → 0 % CPU with no clients connected.
//   - Per-worker epoll: each worker owns its own fd set → no lock contention.
//   - Goroutines are spawned only for sendfd dispatch (off the critical path).
//   - events buffer is 256 entries so we batch more events per EpollWait call,
//     reducing syscall overhead at high connection rates.

package httpmigrate

/*
#include "tls_listener.h"
#include <stdlib.h>
*/
import "C"

import (
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// ── RunVanillaHTTPS ──────────────────────────────────────────────────────────

// RunVanillaHTTPS starts an HTTPS listener on tlsPort using wolfSSL.
// numEpollWorkers goroutines each bind the same port via SO_REUSEPORT; the
// kernel load-balances connections across them (no single-goroutine bottleneck).
// After the TLS handshake, each connection is pushed to a ChanListener so the
// standard http.Server can serve it.
//

func RunVanillaHTTPS(
	tlsPort int,
	certFile, keyFile string,
	handler http.Handler,
	skipTop1 bool,
) error {
	gtwCtx, err := NewWolfSSLGtwCtx(certFile, keyFile)
	if err != nil {
		return fmt.Errorf("[vanilla-https] wolfSSL init: %w", err)
	}
	defer gtwCtx.Free()

	addr := &net.TCPAddr{Port: tlsPort}
	// Buffer of 512: enough to absorb a burst of 512 connections being pushed
	// to ChanListener before the http.Server goroutine accepts them.
	chanLis := NewChanListener(addr, 512)

	// Wrap the handler with timing middleware unless we are in SUM_PROD mode.
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

	// Open numEpollWorkers listen fds on the same port (SO_REUSEPORT).
	listenFDs := make([]int, numEpollWorkers)
	for i := range listenFDs {
		lfd, lerr := rawTCPListen(tlsPort)
		if lerr != nil {
			for j := 0; j < i; j++ {
				_ = syscall.Close(listenFDs[j])
			}
			return fmt.Errorf("[vanilla-https] listen :%d (worker %d): %w", tlsPort, i, lerr)
		}
		listenFDs[i] = lfd
	}

	log.Printf("[vanilla-https] RunVanillaHTTPS listening on :%d (%d workers, SUM_PROD=%v)\n",
		tlsPort, numEpollWorkers, skipTop1)

	var wg sync.WaitGroup
	for _, lfd := range listenFDs {
		wg.Add(1)
		go func(lfd int) {
			defer wg.Done()
			defer syscall.Close(lfd)
			if err := runEpollLoop(gtwCtx, lfd, -1, "", chanLis, "", nil, skipTop1, true); err != nil {
				log.Printf("[vanilla-https] epoll loop error: %v\n", err)
			}
		}(lfd)
	}
	wg.Wait()
	return nil
}

// ── RunLoopHTTPS ─────────────────────────────────────────────────────────────

// RunLoopHTTPS is the HTTPS prototype mode listener.
// numEpollWorkers goroutines each bind the same port via SO_REUSEPORT; the
// kernel load-balances connections across them (no single-goroutine bottleneck).
// Workflow per connection:
//  1. accept4() → non-blocking fd
//  2. wolfssl_accept_start() → epoll EPOLLIN
//  3. wolfssl_handshake_step() per epoll event until complete
//  4. tlsgw_peek_and_export_nb() → EAGAIN loops via EPOLLET, no busy-wait
//     5a. /function/<name> → goroutine for sendfd dispatch (non-blocking)
//     5b. other path       → WolfSSLGtwConn pushed to ChanListener
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

	gtwCtx, err := NewWolfSSLGtwCtx(certFile, keyFile)
	if err != nil {
		return fmt.Errorf("[httpmigrate-https] wolfSSL init: %w", err)
	}
	defer gtwCtx.Free()

	addr := &net.TCPAddr{Port: tlsPort}
	chanLis := NewChanListener(addr, 512)

	// The http.Server here handles non-function paths (/system/*, /ui/*, …)
	// that arrive via the ChanListener after the peek phase determines they are
	// not /function/ requests.
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

	// Open numEpollWorkers listen fds on the same port (SO_REUSEPORT).
	listenFDs := make([]int, numEpollWorkers)
	for i := range listenFDs {
		lfd, lerr := rawTCPListen(tlsPort)
		if lerr != nil {
			for j := 0; j < i; j++ {
				_ = syscall.Close(listenFDs[j])
			}
			return fmt.Errorf("[httpmigrate-https] listen :%d (worker %d): %w", tlsPort, i, lerr)
		}
		listenFDs[i] = lfd
	}

	log.Printf("[httpmigrate-https] RunLoopHTTPS listening on :%d (%d workers)\n",
		tlsPort, numEpollWorkers)

	var wg sync.WaitGroup
	for _, lfd := range listenFDs {
		wg.Add(1)
		go func(lfd int) {
			defer wg.Done()
			defer syscall.Close(lfd)
			if err := runEpollLoop(gtwCtx, lfd, -1, "", chanLis, providerURL, notifier, skipTop1, false); err != nil {
				log.Printf("[httpmigrate-https] epoll loop error: %v\n", err)
			}
		}(lfd)
	}
	wg.Wait()
	return nil
}

// ── runEpollLoop — single shared epoll-driven event loop ─────────────────────
//
// Drives the handshake state machine (and peek/export for prototype mode) for
// ALL connections on a single goroutine.  This eliminates one goroutine and one
// OS thread per connection.
//
// Parameters:
//
//	gtwCtx      — shared wolfSSL context
//	listenFD    — the raw TCP listen fd (SO_REUSEADDR, non-blocking)
//	chanLis     — channel-based net.Listener feeding the http.Server
//	providerURL — faasd-provider URL (prototype mode): the raw fd + exported TLS
//	              state are sent to the provider, which forwards them (with the
//	              function name) to the function's watchdog.
//	notifier    — Prometheus completion callback (prototype mode only)
//	skipTop1    — SUM_PROD mode: skip timing instrumentation on the gateway
//	vanillaMode — true  → RunVanillaHTTPS (no peek, no libtlspeek)
//	              false → RunLoopHTTPS   (peek + export + sendfd)
func runEpollLoop(
	gtwCtx *WolfSSLGtwCtx,
	listenFD int,
	_ int, // (unused) former worker→gateway relay listen fd; the 2nd hop is now done by the faasd-provider
	_ string, // (unused) reserved
	chanLis *ChanListener,
	providerURL string,
	notifier CompletionNotifier,
	skipTop1 bool,
	vanillaMode bool,
) error {
	epollFD, err := syscall.EpollCreate1(0)
	if err != nil {
		return fmt.Errorf("epoll create: %w", err)
	}
	defer syscall.Close(epollFD)

	// Register the listen socket — EPOLLIN fires on every new incoming connection.
	if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, listenFD,
		&syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(listenFD)}); err != nil {
		return fmt.Errorf("epoll add listen fd: %w", err)
	}

	// pending maps client fd → *wolfssl_gtw_conn_t (connections in handshake or peek).
	pending := make(map[int]*C.wolfssl_gtw_conn_t)

	// closePending removes a connection from epoll and frees its wolfSSL state.
	closePending := func(fd int) {
		if conn, ok := pending[fd]; ok {
			_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
			C.wolfssl_conn_free(conn)
			delete(pending, fd)
		}
	}

	// connEvents returns the epoll event mask a pending conn currently needs.
	connEvents := func(conn *C.wolfssl_gtw_conn_t) uint32 {
		evs := uint32(syscall.EPOLLRDHUP | syscall.EPOLLERR)
		if C.wolfssl_conn_want_events(conn) == 2 {
			evs |= syscall.EPOLLOUT // wolfSSL wants to write (handshake message)
		} else {
			evs |= syscall.EPOLLIN // wolfSSL wants to read (peer data)
		}
		return evs
	}

	// events is the EpollWait batch buffer.  256 entries reduces the number of
	// syscalls required at high connection rates compared to the old 128.
	events := make([]syscall.EpollEvent, 256)
	// peekBuf is reused across peek iterations (one connection at a time in the
	// epoll loop) to avoid per-connection allocation of the ~24 KB serial buffer.
	peekBuf := make([]byte, gtwCtx.SerialSize())

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
			evFlags := events[i].Events

			// ── Listen socket: drain the accept queue ────────────────────────
			if fd == listenFD {
				for {
					// Vanilla mode: wolfssl_accept_start_plain — NO keylog callback,
					// no libtlspeek at any point in the connection lifetime.
					// Prototype mode: wolfssl_accept_start — installs keylog callback
					// so session keys are captured for tls_read_peek / export later.
					var conn *C.wolfssl_gtw_conn_t
					if vanillaMode {
						conn = C.wolfssl_accept_start_plain(gtwCtx.ptr, C.int(listenFD))
					} else {
						conn = C.wolfssl_accept_start(gtwCtx.ptr, C.int(listenFD))
					}
					if conn == nil {
						break // EAGAIN — no more pending connections
					}
					clientFD := int(C.wolfssl_conn_fd(conn))
					if clientFD < 0 {
						C.wolfssl_conn_free(conn)
						continue
					}
					if err := syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_ADD, clientFD,
						&syscall.EpollEvent{Events: connEvents(conn), Fd: int32(clientFD)}); err != nil {
						C.wolfssl_conn_free(conn)
						log.Printf("[epoll] EpollCtl ADD client fd=%d failed: %v\n", clientFD, err)
						continue
					}
					pending[clientFD] = conn
				}
				continue
			}

			// ── Client connection: drive handshake then peek ─────────────────
			conn := pending[fd]
			if conn == nil {
				// Stale fd (e.g. removed from pending but still in epoll).
				_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
				_ = syscall.Close(fd)
				continue
			}

			if evFlags&(syscall.EPOLLERR|syscall.EPOLLHUP|syscall.EPOLLRDHUP) != 0 {
				// Peer reset or closed before handshake completed.
				closePending(fd)
				continue
			}

			state := C.wolfssl_conn_state(conn)

			// ── Drive TLS handshake ──────────────────────────────────────────
			if state == C.WOLFSSL_GTW_CONN_HANDSHAKE {
				rc := C.wolfssl_handshake_step(conn)
				if rc == 0 {
					// Handshake in progress — update epoll to the direction
					// wolfSSL currently needs (read or write).
					_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_MOD, fd,
						&syscall.EpollEvent{Events: connEvents(conn), Fd: int32(fd)})
					continue
				}
				if rc < 0 {
					// Fatal handshake error (bad cert, peer reset, etc.).
					closePending(fd)
					continue
				}

				// Handshake complete (rc == 1) → conn transitions to PEEK state.
				// Switch to EPOLLET (edge-triggered) for the peek phase so we do
				// NOT spin when the first HTTP record is not yet fully available —
				// EPOLLET only fires on a new edge (data arrival), so we park here
				// until the client sends its first request.
				_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_MOD, fd,
					&syscall.EpollEvent{
						Events: syscall.EPOLLIN | syscall.EPOLLRDHUP |
							syscall.EPOLLERR | syscall.EPOLLET,
						Fd: int32(fd),
					})

				if vanillaMode {
					// Vanilla: push to ChanListener immediately after handshake.
					// wrapGtwConn transfers ownership of conn (wolfSSL* + fd).
					// Do NOT call wolfssl_conn_free here — wrapGtwConn calls
					// wolfssl_gtw_conn_close() via WolfSSLGtwConn.Close() later.
					_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
					delete(pending, fd)
					rawFD := int(C.wolfssl_conn_take_raw_fd(conn))
					gtwConn := wrapGtwConn(conn, rawFD)
					if pushErr := chanLis.Push(gtwConn); pushErr != nil {
						_ = gtwConn.Close()
					}
					continue
				}

				// Prototype: fall through immediately to the PEEK state so we
				// process any data that arrived in the same TCP segment as the
				// TLS Finished message (common with HTTP/1.1 pipelining).
				state = C.WOLFSSL_GTW_CONN_PEEK
			}

			// ── Drive peek + export (prototype mode only) ────────────────────
			if state == C.WOLFSSL_GTW_CONN_PEEK {
				var cFnName [128]C.char
				var cTop1 C.uint64_t
				var cSerialSz C.int
				cSkip := C.int(0)
				if skipTop1 {
					cSkip = 1
				}

				rc := C.tlsgw_peek_and_export_nb(
					conn,
					&cFnName[0],
					128,
					&cTop1,
					unsafe.Pointer(&peekBuf[0]),
					&cSerialSz,
					cSkip,
				)

				switch {
				case rc == 0:
					// EAGAIN — first HTTP record not yet arrived.
					// EPOLLET is active: we do not re-arm, just wait for the
					// next rising edge (more data from client).

				case rc > 0:
					// /function/<name> path — wolfSSL detached, raw fd returned.
					// conn was freed inside tlsgw_peek_and_export_nb (rc > 0 path).
					_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
					delete(pending, fd)
					rawFD := int(rc)
					fnName := C.GoString(&cFnName[0])
					var top1Val uint64
					if !skipTop1 {
						top1Val = uint64(cTop1)
						// [MICROBENCH] top1 (gateway, HTTPS) logged DIRECTLY. The
						// watchdog logs top2 on its side and migration_ns = top2 -
						// top1 is computed offline. top1 is no longer used for the
						// measurement; it stays in the payload only as the wire
						// "pre-routed" flag (top1_set) that the watchdog uses to
						// skip the owner peek on provider-re-routed connections.
						if microbenchOn {
							log.Printf("[MICROBENCH] proto_top1_ns=%d\n", top1Val)
						}
					}
					// Copy serial from peekBuf before the next epoll iteration
					// overwrites it (peekBuf is reused across connections).
					serial := make([]byte, int(cSerialSz))
					copy(serial, peekBuf[:int(cSerialSz)])
					payload := NewPayloadHTTPS(top1Val, fnName, serial)
					go func(rfd int, fn string, pl *KAPayloadHTTPS) {
						if err := dispatchMigrateHTTPS(rfd, fn, pl, providerURL, notifier); err != nil {
							log.Printf("[epoll-https] dispatch FAILED fn=%s: %v\n", fn, err)
							_ = syscall.Close(rfd)
						}
					}(rawFD, fnName, payload)

				case rc == -1:
					// Not a /function/ path (e.g. /system/info) — push to ChanListener
					// so the http.Server serves it via the normal OpenFaaS router.
					_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
					delete(pending, fd)
					rawFD := int(C.wolfssl_conn_take_raw_fd(conn))
					gtwConn := wrapGtwConn(conn, rawFD)
					if pushErr := chanLis.Push(gtwConn); pushErr != nil {
						_ = gtwConn.Close()
					}

				default: // rc <= -2: fatal error — conn already freed inside C
					log.Printf("[epoll-https] peek/export FAILED fd=%d rc=%d — dropping connection\n", fd, int(rc))
					_ = syscall.EpollCtl(epollFD, syscall.EPOLL_CTL_DEL, fd, nil)
					delete(pending, fd)
					// Do NOT call wolfssl_conn_free — conn was freed inside C.
				}
			}
		}
	}
}

// ── wrapGtwConn ──────────────────────────────────────────────────────────────

// wrapGtwConn converts a *C.wolfssl_gtw_conn_t (handshake complete) into a
// WolfSSLGtwConn that implements net.Conn for the http.Server.
//
// A small per-conn epoll fd is created for the Read/Write blocking path.
// This avoids calling the blocking goroutine scheduler directly and instead
// uses a 500 ms EpollWait timeout so deadline checks are prompt.
//
// These connections are long-lived HTTP sessions for /system/* and UI traffic —
// their count is typically low (< 10 at any time), so the per-conn epoll
// overhead is negligible compared to the reduction in goroutine contention.
func wrapGtwConn(cconn *C.wolfssl_gtw_conn_t, fd int) *WolfSSLGtwConn {
	// The fd must be non-blocking for wolfSSL's WANT_READ/WANT_WRITE model.
	syscall.SetNonblock(fd, true)

	epfd, _ := syscall.EpollCreate1(0)
	if epfd >= 0 {
		var epev syscall.EpollEvent
		epev.Fd = int32(fd)
		epev.Events = syscall.EPOLLIN | syscall.EPOLLRDHUP | syscall.EPOLLERR
		_ = syscall.EpollCtl(epfd, syscall.EPOLL_CTL_ADD, fd, &epev)
	}
	return &WolfSSLGtwConn{
		ptr:        cconn,
		fd:         fd,
		epfd:       epfd,
		localAddr:  gtwSocketAddr(fd, false),
		remoteAddr: gtwSocketAddr(fd, true),
	}
}

// ── rawTCPListen ─────────────────────────────────────────────────────────────

// numEpollWorkers is the number of independent goroutines (each with its own
// epoll fd and listen fd) that accept and process TLS connections.
// SO_REUSEPORT lets all workers bind the same port; the kernel distributes
// new connections across the workers so no single goroutine is the bottleneck.
const numEpollWorkers = 4

// rawTCPListen creates a non-blocking TCP listen socket on port.
// Tries IPv6 dual-stack first (IPV6_V6ONLY=0 accepts both IPv4 and IPv6),
// falls back to IPv4-only if IPv6 is unavailable.
//
// SO_REUSEPORT is set so that multiple goroutines can each bind the same port.
// The kernel load-balances incoming connections across all bound sockets,
// removing the single-goroutine accept bottleneck at high connection rates.
//
// CRITICAL: the listen fd MUST be non-blocking so that the accept4() drain
// loop inside wolfssl_accept_start / wolfssl_accept_start_plain returns EAGAIN
// (errno=EAGAIN) when no more connections are pending, instead of blocking
// the entire epoll loop goroutine.
// Note: SOCK_NONBLOCK in accept4() only affects the *accepted* socket;
// it does NOT make the listen fd itself non-blocking.
func rawTCPListen(port int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_INET6, syscall.SOCK_STREAM, 0)
	if err != nil {
		// IPv6 not available — fall back to IPv4 only.
		fd, err = syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
		if err != nil {
			return -1, fmt.Errorf("socket: %w", err)
		}
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEPORT, 1)
		var sa4 syscall.SockaddrInet4
		sa4.Port = port
		if err := syscall.Bind(fd, &sa4); err != nil {
			_ = syscall.Close(fd)
			return -1, fmt.Errorf("bind IPv4 :%d: %w", port, err)
		}
	} else {
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEPORT, 1)
		// IPV6_V6ONLY=0: the IPv6 socket also accepts IPv4-mapped addresses,
		// giving a single socket that handles both protocol families.
		_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_IPV6, syscall.IPV6_V6ONLY, 0)
		var sa6 syscall.SockaddrInet6
		sa6.Port = port
		if err := syscall.Bind(fd, &sa6); err != nil {
			_ = syscall.Close(fd)
			return -1, fmt.Errorf("bind IPv6 :%d: %w", port, err)
		}
	}

	// backlog=4096 allows up to 4096 connections in the kernel SYN queue.
	// At high RPS, a small backlog causes ECONNREFUSED during bursts.
	if err := syscall.Listen(fd, 4096); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("listen :%d: %w", port, err)
	}

	// Set non-blocking on the listen fd so the accept4() drain loop returns
	// EAGAIN when no more connections are waiting, without blocking.
	if err := syscall.SetNonblock(fd, true); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("setnonblock listen :%d: %w", port, err)
	}
	return fd, nil
}
