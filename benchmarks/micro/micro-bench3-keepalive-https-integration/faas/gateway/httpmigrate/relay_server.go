//go:build linux

package httpmigrate

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// ── Relay socket registry ─────────────────────────────────────────────────────

// relayState tracks which container IPs already have a relay socket open.
// Guarded by relayMu.  Both HTTP and HTTPS relay helpers share this map —
// since each gateway process runs in exactly one mode (HTTP or HTTPS, never
// both on the same port), there is no conflict.
var (
	relayMu    sync.Mutex
	relayKnown = map[string]bool{}
)

// ── EnsureRelaySocket (HTTP mode) ────────────────────────────────────────────

// EnsureRelaySocket creates the per-container HTTP relay socket at
// <SocketDir>/<ip>-relay.sock if it does not already exist, then spawns
// a goroutine to service it.  The providerURL and notifier are forwarded
// so the relay goroutine can re-dispatch returned fds.
//
// This function is idempotent: calling it for the same IP more than once is safe.
func EnsureRelaySocket(ip, providerURL string, notifier CompletionNotifier) {
	relayMu.Lock()
	if relayKnown[ip] {
		relayMu.Unlock()
		return
	}
	relayKnown[ip] = true
	relayMu.Unlock()

	sockPath := filepath.Join(SocketDir, ip+"-relay.sock")
	_ = os.Remove(sockPath)

	listenFD, err := bindUnixSocket(sockPath, 512)
	if err != nil {
		log.Printf("[relay] failed to create relay socket %s: %v\n", sockPath, err)
		relayMu.Lock()
		delete(relayKnown, ip)
		relayMu.Unlock()
		return
	}
	_ = os.Chmod(sockPath, 0o777)

	log.Printf("[relay] relay socket ready: %s\n", sockPath)
	go relayListenLoop(listenFD, providerURL, notifier)
}

// ── relayListenLoop (HTTP mode) ───────────────────────────────────────────────

// relayListenLoop services a relay socket: it accepts connections from function
// workers that detected a wrong-owner keep-alive request and are returning the
// fd with a payload that already has top1_rdtsc/cntfrq and target_function set.
// One goroutine is spawned per accepted connection to call recvfd1WithState.
func relayListenLoop(listenFD int, providerURL string, notifier CompletionNotifier) {
	defer syscall.Close(listenFD)

	for {
		connFD, _, err := syscall.Accept(listenFD)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			log.Printf("[relay] accept error: %v\n", err)
			return
		}

		go func(cFD int) {
			defer syscall.Close(cFD)

			buf := make([]byte, KAPayloadSize)
			clientFD, relayErr := recvfd1WithState(cFD, buf)
			if relayErr != nil {
				log.Printf("[relay] recvfd1 error: %v\n", relayErr)
				return
			}

			payload := UnmarshalPayload(buf)
			if payload == nil || payload.Magic != KAMagic {
				log.Printf("[relay] bad magic or short payload\n")
				_ = syscall.Close(clientFD)
				return
			}

			targetFn := payload.Target()
			if targetFn == "" {
				log.Printf("[relay] empty target function in relay payload\n")
				_ = syscall.Close(clientFD)
				return
			}

			if dispatchErr := dispatchMigrate(clientFD, targetFn, payload, providerURL, notifier); dispatchErr != nil {
				log.Printf("[relay] dispatch failed for fn=%s: %v\n", targetFn, dispatchErr)
				_ = syscall.Close(clientFD)
			}
		}(connFD)
	}
}

// ── dispatchMigrate (HTTP mode) ───────────────────────────────────────────────

// dispatchMigrate resolves the container IP for targetFn, creates a fresh OS
// pipe for Prometheus completion notification, and sends [clientFD, pipeWriteFD]
// + payload to the container's watchdog Unix socket via SCM_RIGHTS.
//
// IMPORTANT: the readCompletionPipe goroutine is started ONLY after
// sendfd2WithState succeeds.  If dispatch fails, pipeWriteFD is closed by
// sendfd2WithState's deferred Close without writing.  The old code started the
// goroutine first — readFull then saw (0, nil) from syscall.Read and spun
// forever, accumulating spinning goroutines per failed dispatch.
func dispatchMigrate(clientFD int, targetFn string, payload *KAPayload, providerURL string, notifier CompletionNotifier) error {
	ip, resolveErr := ResolveContainerIP(targetFn, providerURL)
	if resolveErr != nil {
		log.Printf("[dispatch] ERROR step1 resolve IP fn=%s: %v\n", targetFn, resolveErr)
		return fmt.Errorf("resolve IP for %s: %w", targetFn, resolveErr)
	}

	EnsureRelaySocket(ip, providerURL, notifier)

	var pipeEnds [2]int
	if err := syscall.Pipe(pipeEnds[:]); err != nil {
		log.Printf("[dispatch] ERROR step3 pipe: %v\n", err)
		return fmt.Errorf("os.Pipe: %w", err)
	}
	pipeReadFD := pipeEnds[0]
	pipeWriteFD := pipeEnds[1]

	payload.SetTarget(targetFn)

	sockPath := filepath.Join(SocketDir, ip+".sock")
	watchdogFD, err := connectUnixSocket(sockPath)
	if err != nil {
		log.Printf("[dispatch] ERROR step5 connect watchdog %s: %v\n", sockPath, err)
		_ = syscall.Close(pipeReadFD)
		_ = syscall.Close(pipeWriteFD)
		return fmt.Errorf("connect watchdog %s: %w", sockPath, err)
	}
	defer syscall.Close(watchdogFD)

	if err := sendfd2WithState(watchdogFD, clientFD, pipeWriteFD, payload.Marshal()); err != nil {
		log.Printf("[dispatch] ERROR step6 sendfd2: %v\n", err)
		_ = syscall.Close(pipeReadFD)
		return fmt.Errorf("sendfd2 to watchdog: %w", err)
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

// ── IP cache ─────────────────────────────────────────────────────────────────

// ipCacheEntry stores a resolved container IP together with the time it was
// cached.  Entries expire after ipCacheTTL so that if a function container
// restarts with a new CNI IP, the gateway picks up the new address.
type ipCacheEntry struct {
	ip       string
	cachedAt time.Time
}

const ipCacheTTL = 5 * time.Minute

var (
	ipCacheMu sync.RWMutex
	ipCache   = make(map[string]ipCacheEntry)
)

// ResolveContainerIP queries the faasd provider for the CNI IP of a running
// function container.  Returns the IP string (e.g. "10.62.0.5").
//
// Results are cached for ipCacheTTL (5 minutes).  The TTL prevents serving
// a stale IP if a container restarts and receives a new address from the CNI
// plugin.  Without a TTL the cache would grow unbounded and stale entries
// would silently cause dispatch failures after container restarts.
func ResolveContainerIP(fnName, providerURL string) (string, error) {
	// Fast path: read from cache (RLock — concurrent reads are safe).
	ipCacheMu.RLock()
	entry, ok := ipCache[fnName]
	ipCacheMu.RUnlock()
	if ok && entry.ip != "" && time.Since(entry.cachedAt) < ipCacheTTL {
		return entry.ip, nil
	}

	// Slow path: query the provider.
	baseURL := strings.TrimRight(providerURL, "/")
	url := baseURL + "/system/function-ip/" + fnName

	log.Printf("[resolve-ip] GET %s\n", url)
	resp, err := http.Get(url) //nolint:gosec — internal service call to faasd provider
	if err != nil {
		log.Printf("[resolve-ip] ERROR http.Get %s: %v\n", url, err)
		return "", fmt.Errorf("GET %s: %w", url, err)
	}
	defer resp.Body.Close()

	log.Printf("[resolve-ip] response status=%d url=%s\n", resp.StatusCode, url)

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		log.Printf("[resolve-ip] ERROR non-200 status=%d body=%s\n", resp.StatusCode, string(body))
		return "", fmt.Errorf("GET %s: status %d: %s", url, resp.StatusCode, string(body))
	}

	var result struct {
		IP string `json:"ip"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		log.Printf("[resolve-ip] ERROR decode response: %v\n", err)
		return "", fmt.Errorf("decode IP response: %w", err)
	}
	if result.IP == "" {
		log.Printf("[resolve-ip] ERROR empty IP in response for fn=%s\n", fnName)
		return "", fmt.Errorf("empty IP in response from provider for: %s", fnName)
	}

	// Store with a timestamp so the TTL check above can expire it later.
	ipCacheMu.Lock()
	ipCache[fnName] = ipCacheEntry{ip: result.IP, cachedAt: time.Now()}
	ipCacheMu.Unlock()

	log.Printf("[resolve-ip] OK fn=%s -> ip=%s\n", fnName, result.IP)
	return result.IP, nil
}

// ── Unix socket helpers ───────────────────────────────────────────────────────

// bindUnixSocket creates a SOCK_SEQPACKET Unix domain socket, binds it to
// path, and calls listen(2) with the given backlog.  Returns the listening fd.
// SOCK_SEQPACKET provides reliable, ordered, connection-oriented datagrams —
// ideal for sending exactly one payload + SCM_RIGHTS per sendmsg call.
func bindUnixSocket(path string, backlog int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	_ = os.Remove(path)
	addr := &syscall.SockaddrUnix{Name: path}
	if err := syscall.Bind(fd, addr); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("bind %s: %w", path, err)
	}
	if err := syscall.Listen(fd, backlog); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("listen %s: %w", path, err)
	}
	return fd, nil
}

// connectUnixSocket connects a SOCK_SEQPACKET Unix domain socket to path.
// Returns the connected fd or an error if the socket does not exist or the
// watchdog is not listening yet.
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
