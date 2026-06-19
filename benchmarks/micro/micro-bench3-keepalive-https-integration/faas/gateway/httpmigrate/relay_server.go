//go:build linux

package httpmigrate

import (
	"encoding/binary"
	"fmt"
	"io"
	"log"
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
// sendfd2WithState's deferred Close without writing.  Starting the goroutine
// first would make readFull spin on an EOF pipe.
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
// written by the function worker after it has sent the HTTP response.
// It then calls notifier with the elapsed duration for Prometheus.
func readCompletionPipe(pipeReadFD int, fnName string, start time.Time, notifier CompletionNotifier) {
	defer syscall.Close(pipeReadFD)

	var tsBuf [8]byte
	n, err := readFull(pipeReadFD, tsBuf[:])
	if err != nil || n < 8 {
		return
	}
	_ = binary.LittleEndian.Uint64(tsBuf[:]) // worker-side timestamp (unused here)

	elapsed := time.Since(start)
	if notifier != nil {
		notifier(fnName, elapsed)
	}
}

// ── readFull ─────────────────────────────────────────────────────────────────

// readFull reads exactly len(buf) bytes from a blocking fd (typically a pipe).
// syscall.Read returns (0, nil) when the write-end of a pipe is closed without
// writing — that is EOF on a pipe.  Without this check the old code would spin
// forever at 100 % CPU per stuck goroutine.
func readFull(fd int, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := syscall.Read(fd, buf[total:])
		if n > 0 {
			total += n
		}
		if n == 0 && err == nil {
			// Pipe write-end closed (EOF): no more data will ever arrive.
			return total, io.EOF
		}
		if err != nil {
			return total, err
		}
	}
	return total, nil
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
