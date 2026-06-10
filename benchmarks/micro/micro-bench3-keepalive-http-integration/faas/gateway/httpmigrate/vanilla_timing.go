//go:build linux

// Package httpmigrate — vanilla_timing.go
//
// Non-intrusive top1 timestamp capture for the vanilla (standard HTTP proxy)
// OpenFaaS path.
//
// Two pieces:
//
//  1. StartVanillaTimingObserver() — returns a ConnState hook for http.Server.
//     For every new connection, spawns a goroutine that calls unix.Poll(POLLIN)
//     without reading any data.  Stamps top1 = CLOCK_MONOTONIC_RAW the instant
//     the kernel reports bytes available, then stores it in vanillaTop1Map.
//
//  2. VanillaTimingMiddleware() — http.Handler middleware.
//     Reads top1 from vanillaTop1Map (LoadAndDelete) and injects it as the
//     request header  "X-Top1-Rdtsc: <ns>".  The OpenFaaS reverse proxy and
//     the watchdog http-mode proxy forward all headers transparently, so the
//     header reaches the vanilla function server with no other change.
//
// The OpenFaaS workflow (accept → parse → proxy → scale → auth) is untouched.

package httpmigrate

import (
	"fmt"
	"log"
	"net"
	"net/http"
	"strconv"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

// vanillaTop1Map: remoteAddr (string) → top1 (uint64 nanoseconds).
// Written by observeVanillaConn, consumed by VanillaTimingMiddleware.
var vanillaTop1Map sync.Map

// vanillaInsert tracks insertion timestamps for stale-entry cleanup.
var (
	vanillaInsertMu    sync.Mutex
	vanillaInsertTimes = make(map[string]time.Time)
)

// StartVanillaTimingObserver returns a ConnState callback to assign to
// http.Server.ConnState.  Call once at gateway startup:
//
//	s.ConnState = httpmigrate.StartVanillaTimingObserver()
func StartVanillaTimingObserver() func(net.Conn, http.ConnState) {
	// Background goroutine: sweep stale entries every 30 s.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for range t.C {
			vanillaInsertMu.Lock()
			now := time.Now()
			for addr, insertedAt := range vanillaInsertTimes {
				if now.Sub(insertedAt) > 60*time.Second {
					vanillaTop1Map.Delete(addr)
					delete(vanillaInsertTimes, addr)
					log.Printf("[vanilla-timing] swept stale entry for %s\n", addr)
				}
			}
			vanillaInsertMu.Unlock()
		}
	}()

	return func(conn net.Conn, state http.ConnState) {
		switch state {
		case http.StateNew:
			// Stamp top1 SYNCHRONOUSLY — no goroutine.
			// StateNew fires before the handler goroutine calls the middleware,
			// so the map entry is guaranteed to exist when the handler runs.
			// Data has already arrived in the kernel buffer by the time StateNew
			// fires (client connected + sent headers in one RTT), so accuracy
			// is within a few microseconds.
			top1 := getMonotonicNs()
			storeVanillaTop1(conn.RemoteAddr().String(), top1)

		case http.StateIdle:
			// Stamp top1 SYNCHRONOUSLY — no goroutine.
			//
			// A goroutine+Poll approach here races against the handler goroutine:
			//   1. If the goroutine stores AFTER the handler ran → top1 = 0
			//   2. The stale entry is then consumed by the NEXT request →
			//      wrong delta_ns (e.g. 195 ms instead of 35 ms).
			//
			// Stamping synchronously in StateIdle means top1 is captured
			// just as the previous response finishes sending (slightly before
			// the next request bytes arrive). The systematic offset is small
			// (≈ client RTT ≈ 1-2 ms) vs measured latencies of 25-200 ms,
			// and it is CONSISTENT across all keep-alive requests, making
			// the vanilla vs prototype comparison valid.
			top1 := getMonotonicNs()
			storeVanillaTop1(conn.RemoteAddr().String(), top1)
		}
	}
}

// observeVanillaConn polls the connection fd for POLLIN readiness using
// golang.org/x/sys/unix.Poll (not syscall.Poll, which is not available
// on all Linux architectures in the standard library).
// Stamps top1 and stores it in vanillaTop1Map when data arrives.
func observeVanillaConn(conn net.Conn) {
	remoteAddr := conn.RemoteAddr().String()

	fd, ok := getRawFD(conn)
	if !ok {
		// Cannot extract fd: use current time as fallback.
		top1 := getMonotonicNs()
		storeVanillaTop1(remoteAddr, top1)
		return
	}

	// unix.Poll(fd, POLLIN, 5000ms) — waits for kernel to report data ready.
	// Does NOT call read() — zero bytes consumed.
	fds := []unix.PollFd{{Fd: int32(fd), Events: unix.POLLIN}}
	n, err := unix.Poll(fds, 5000)
	if err != nil || n <= 0 {
		top1 := getMonotonicNs()
		storeVanillaTop1(remoteAddr, top1)
		return
	}

	// POLLIN fired: data is in kernel buffer. Stamp top1 immediately.
	top1 := getMonotonicNs()
	storeVanillaTop1(remoteAddr, top1)
	log.Printf("[vanilla-timing] top1 stamped addr=%s top1_ns=%d\n", remoteAddr, top1)
}

// storeVanillaTop1 writes to both vanillaTop1Map and vanillaInsertTimes.
func storeVanillaTop1(remoteAddr string, top1 uint64) {
	vanillaTop1Map.Store(remoteAddr, top1)
	vanillaInsertMu.Lock()
	vanillaInsertTimes[remoteAddr] = time.Now()
	vanillaInsertMu.Unlock()
}

// getRawFD extracts the underlying OS fd from a net.Conn.
// Works for *net.TCPConn and any type implementing syscall.Conn.
func getRawFD(conn net.Conn) (uintptr, bool) {
	type syscallConnIface interface {
		SyscallConn() (syscall.RawConn, error)
	}
	sc, ok := conn.(syscallConnIface)
	if !ok {
		return 0, false
	}
	rawConn, err := sc.SyscallConn()
	if err != nil {
		return 0, false
	}
	var fd uintptr
	if ctrlErr := rawConn.Control(func(f uintptr) { fd = f }); ctrlErr != nil {
		return 0, false
	}
	return fd, fd != 0
}

// VanillaTimingMiddleware wraps an http.Handler.
// For every request it injects the pre-stamped top1 as "X-Top1-Rdtsc" (ns).
// The entry is consumed (LoadAndDelete) so the map never grows unboundedly.
//
// Usage in main.go (vanilla path only):
//
//	s.Handler = httpmigrate.VanillaTimingMiddleware(r)
func VanillaTimingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if val, loaded := vanillaTop1Map.LoadAndDelete(r.RemoteAddr); loaded {
			top1 := val.(uint64)
			r.Header.Set("X-Top1-Rdtsc", strconv.FormatUint(top1, 10))
			// Also clean up insertion-time tracking.
			vanillaInsertMu.Lock()
			delete(vanillaInsertTimes, r.RemoteAddr)
			vanillaInsertMu.Unlock()
			log.Printf("[vanilla-timing] injected X-Top1-Rdtsc=%d addr=%s path=%s\n",
				top1, r.RemoteAddr, r.URL.Path)
		}
		next.ServeHTTP(w, r)
	})
}

// formatTop1 is a small helper used in tests and logging.
func formatTop1(ns uint64) string {
	return fmt.Sprintf("%d", ns)
}
