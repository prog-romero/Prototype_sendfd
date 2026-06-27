//go:build linux

package httpmigrate

import (
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"os"
	"syscall"
	"time"
)

// ── dispatchMigrate (HTTP mode) ───────────────────────────────────────────────

// dispatchMigrate hands the client connection off to the faasd provider over
// ProviderSock.  In the provider-routed data path the gateway no longer resolves
// the container IP nor talks to the watchdog directly: it stamps the target
// function name into the payload and sends [clientFD, pipeWriteFD] to the
// provider, which resolves the IP in-process and forwards to the correct
// watchdog.  The completion pipe stays owned by the gateway so Prometheus
// metrics are reported here (option A: unchanged metrics path).
//
// IMPORTANT: the readCompletionPipe goroutine is started ONLY after
// sendfd2WithState succeeds.  If dispatch fails, pipeWriteFD is closed by
// sendfd2WithState's deferred Close without writing — starting the goroutine
// first would make it wait pointlessly on an already-EOF pipe.
//
// providerURL is retained in the signature for source compatibility but is no
// longer used — the provider socket path is fixed (ProviderSock).
func dispatchMigrate(clientFD int, targetFn string, payload *KAPayload, providerURL string, notifier CompletionNotifier) error {
	_ = providerURL

	payload.SetTarget(targetFn)

	var pipeEnds [2]int
	if err := syscall.Pipe(pipeEnds[:]); err != nil {
		log.Printf("[dispatch] ERROR pipe: %v\n", err)
		return fmt.Errorf("os.Pipe: %w", err)
	}
	pipeReadFD := pipeEnds[0]
	pipeWriteFD := pipeEnds[1]

	providerFD, err := connectUnixSocket(ProviderSock)
	if err != nil {
		log.Printf("[dispatch] ERROR connect provider %s: %v\n", ProviderSock, err)
		_ = syscall.Close(pipeReadFD)
		_ = syscall.Close(pipeWriteFD)
		return fmt.Errorf("connect provider %s: %w", ProviderSock, err)
	}
	defer syscall.Close(providerFD)

	if err := sendfd2WithState(providerFD, clientFD, pipeWriteFD, payload.Marshal()); err != nil {
		log.Printf("[dispatch] ERROR sendfd2: %v\n", err)
		_ = syscall.Close(pipeReadFD)
		return fmt.Errorf("sendfd2 to provider: %w", err)
	}

	start := time.Now()
	if notifier != nil {
		go readCompletionPipe(pipeReadFD, targetFn, start, notifier)
	} else {
		go func() { syscall.Close(pipeReadFD) }()
	}

	return nil
}

// ── readCompletionPipe ────────────────────────────────────────────────────────

// readCompletionPipe reads an 8-byte little-endian uint64 timestamp (nanoseconds)
// written by the function worker after it has sent the HTTP response, then calls
// notifier with the elapsed duration for Prometheus.
//
// SCALABILITY — why the Go netpoller and a deadline are used here:
//
// One readCompletionPipe goroutine runs per in-flight migrated request. The old
// implementation did a BLOCKING syscall.Read on the raw pipe fd. A blocking
// syscall is NOT integrated with the Go scheduler's network poller, so each
// goroutine waiting on the pipe pinned a dedicated OS thread (M) for the whole
// duration of the request. Under load this exploded the OS-thread count: the
// gateway CPU climbed (scheduler/sysmon overhead) and requests started failing
// even though the application CPU was not saturated — and because Go parks
// (never destroys) those threads, CPU stayed high run-after-run until a restart.
//
// Wrapping the fd in an *os.File makes Read go through the Go netpoller: the
// goroutine is suspended WITHOUT holding an OS thread. A read deadline
// guarantees the goroutine (and the fd) are always released even if the worker
// never signals — so there is no goroutine/fd leak.
func readCompletionPipe(pipeReadFD int, fnName string, start time.Time, notifier CompletionNotifier) {
	// Non-blocking + *os.File ⇒ reads are serviced by the netpoller (epoll),
	// not by a blocked OS thread.
	if err := syscall.SetNonblock(pipeReadFD, true); err != nil {
		_ = syscall.Close(pipeReadFD)
		return
	}
	f := os.NewFile(uintptr(pipeReadFD), "completion-pipe")
	if f == nil {
		_ = syscall.Close(pipeReadFD)
		return
	}
	defer f.Close() // closes the underlying pipeReadFD

	// Bound the wait: the worker writes the timestamp as soon as it has sent the
	// response; if it never does (crash / stuck), we still return after the
	// gateway's upstream timeout window instead of leaking the goroutine + fd.
	_ = f.SetReadDeadline(time.Now().Add(65 * time.Second))

	var tsBuf [8]byte
	if _, err := io.ReadFull(f, tsBuf[:]); err != nil {
		return
	}
	_ = binary.LittleEndian.Uint64(tsBuf[:]) // worker-side timestamp (unused here)

	elapsed := time.Since(start)
	if notifier != nil {
		notifier(fnName, elapsed)
	}
}

// ── Unix socket helper ────────────────────────────────────────────────────────

// connectUnixSocket connects a SOCK_SEQPACKET Unix domain socket to path.
// Returns the connected fd or an error if the socket does not exist or the peer
// (the provider) is not listening yet.
func connectUnixSocket(path string) (int, error) {
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	addr := &syscall.SockaddrUnix{Name: path}
	if err := syscall.Connect(fd, addr); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("connect %s: %w", path, err)
	}
	return fd, nil
}

// ── CompletionNotifier ────────────────────────────────────────────────────────

// CompletionNotifier is a callback invoked when a function request completes.
// Used to record Prometheus metrics via the gateway's standard HTTPNotifier chain.
type CompletionNotifier func(functionName string, duration time.Duration)
