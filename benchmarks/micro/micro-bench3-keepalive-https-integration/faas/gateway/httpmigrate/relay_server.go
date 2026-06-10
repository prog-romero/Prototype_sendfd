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

// relayState tracks per-container relay sockets that the gateway has opened.
// Key = container IP address.
var (
	relayMu    sync.Mutex
	relayKnown = map[string]bool{}
)

// EnsureRelaySocket creates the per-container relay socket at
// <SocketDir>/<ip>-relay.sock if it does not already exist, then spawns
// a goroutine to service it.  The providerURL and notifier are forwarded so
// the relay goroutine can re-dispatch returned FDs.
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

// relayListenLoop services a relay socket: it accepts connections from function
// workers that detected a wrong-owner keepalive request and are returning the
// FD with a payload that already has top1_rdtsc/cntfrq and target_function set.
func relayListenLoop(listenFD int, providerURL string, notifier CompletionNotifier) {
	defer syscall.Close(listenFD)

	buf := make([]byte, KAPayloadSize)

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

			log.Printf("[relay] re-dispatching fd to fn=%s\n", targetFn)

			if dispatchErr := dispatchMigrate(clientFD, targetFn, payload, providerURL, notifier); dispatchErr != nil {
				log.Printf("[relay] dispatch failed for fn=%s: %v\n", targetFn, dispatchErr)
				_ = syscall.Close(clientFD)
			}
		}(connFD)
	}
}

// dispatchMigrate resolves the container IP for targetFn, creates a fresh OS
// pipe for Prometheus notification, and sends [clientFD, pipeWriteFD] + payload
// to the container's watchdog socket.  Exported so that loop.go can call it too.
func dispatchMigrate(clientFD int, targetFn string, payload *KAPayload, providerURL string, notifier CompletionNotifier) error {
	log.Printf("[dispatch] START fn=%s clientFD=%d providerURL=%s\n", targetFn, clientFD, providerURL)

	// 1. Resolve container IP.
	log.Printf("[dispatch] step1: resolving container IP for fn=%s\n", targetFn)
	ip, resolveErr := ResolveContainerIP(targetFn, providerURL)
	if resolveErr != nil {
		log.Printf("[dispatch] ERROR step1 resolve IP fn=%s: %v\n", targetFn, resolveErr)
		return fmt.Errorf("resolve IP for %s: %w", targetFn, resolveErr)
	}
	log.Printf("[dispatch] step1 OK: fn=%s -> ip=%s\n", targetFn, ip)

	// 2. Ensure this container has a relay socket registered.
	log.Printf("[dispatch] step2: ensuring relay socket for ip=%s\n", ip)
	EnsureRelaySocket(ip, providerURL, notifier)

	// 3. Create a pipe for Prometheus completion notification.
	log.Printf("[dispatch] step3: creating completion pipe\n")
	var pipeEnds [2]int
	if err := syscall.Pipe(pipeEnds[:]); err != nil {
		log.Printf("[dispatch] ERROR step3 pipe: %v\n", err)
		return fmt.Errorf("os.Pipe: %w", err)
	}
	pipeReadFD := pipeEnds[0]
	pipeWriteFD := pipeEnds[1]
	log.Printf("[dispatch] step3 OK: pipeReadFD=%d pipeWriteFD=%d\n", pipeReadFD, pipeWriteFD)

	// 4. Start goroutine to read completion notification from pipeReadFD.
	start := time.Now()
	if notifier != nil {
		go readCompletionPipe(pipeReadFD, targetFn, start, notifier)
	} else {
		go func() { syscall.Close(pipeReadFD) }()
	}

	// 5. Build/update the payload (keep top1 and cntfrq from original payload).
	payload.SetTarget(targetFn)

	// 6. Connect to watchdog's UDS and send [clientFD, pipeWriteFD] + payload.
	sockPath := filepath.Join(SocketDir, ip+".sock")
	log.Printf("[dispatch] step6: connecting to watchdog socket=%s\n", sockPath)
	watchdogFD, err := connectUnixSocket(sockPath)
	if err != nil {
		log.Printf("[dispatch] ERROR step6 connect watchdog %s: %v\n", sockPath, err)
		_ = syscall.Close(pipeReadFD)
		_ = syscall.Close(pipeWriteFD)
		return fmt.Errorf("connect watchdog %s: %w", sockPath, err)
	}
	defer syscall.Close(watchdogFD)
	log.Printf("[dispatch] step6 OK: watchdogFD=%d socket=%s\n", watchdogFD, sockPath)

	log.Printf("[dispatch] step7: sendfd2 clientFD=%d pipeWriteFD=%d -> watchdogFD=%d\n", clientFD, pipeWriteFD, watchdogFD)
	if err := sendfd2WithState(watchdogFD, clientFD, pipeWriteFD, payload.Marshal()); err != nil {
		log.Printf("[dispatch] ERROR step7 sendfd2: %v\n", err)
		_ = syscall.Close(pipeReadFD)
		return fmt.Errorf("sendfd2 to watchdog: %w", err)
	}
	log.Printf("[dispatch] step7 OK: FDs transferred to watchdog fn=%s\n", targetFn)
	// clientFD and pipeWriteFD are now owned by the watchdog/function worker.

	log.Printf("[dispatch] DONE fn=%s ip=%s\n", targetFn, ip)
	return nil
}

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
	_ = binary.LittleEndian.Uint64(tsBuf[:]) // function-side timestamp (unused)

	elapsed := time.Since(start)
	if notifier != nil {
		notifier(fnName, elapsed)
	}
}

// readFull reads exactly len(buf) bytes from an fd.
func readFull(fd int, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := syscall.Read(fd, buf[total:])
		if n > 0 {
			total += n
		}
		if err != nil {
			return total, err
		}
	}
	return total, nil
}

var (
	ipCacheMu sync.RWMutex
	ipCache   = make(map[string]string)
)

// ResolveContainerIP queries the faasd provider for the CNI IP of a running
// function container.  Returns the IP string (e.g. "10.62.0.5").
func ResolveContainerIP(fnName, providerURL string) (string, error) {
	ipCacheMu.RLock()
	ip, ok := ipCache[fnName]
	ipCacheMu.RUnlock()
	if ok && ip != "" {
		return ip, nil
	}

	// Strip any trailing slash from providerURL to avoid double-slash URLs
	// (e.g. "http://faasd-provider:8081/" + "/system/..." = "//system/...").
	baseURL := strings.TrimRight(providerURL, "/")
	url := baseURL + "/system/function-ip/" + fnName

	log.Printf("[resolve-ip] GET %s\n", url)
	resp, err := http.Get(url) //nolint:gosec — internal service call
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

	ipCacheMu.Lock()
	ipCache[fnName] = result.IP
	ipCacheMu.Unlock()

	log.Printf("[resolve-ip] OK fn=%s -> ip=%s\n", fnName, result.IP)
	return result.IP, nil
}

// bindUnixSocket creates a SOCK_SEQPACKET Unix domain socket, binds it to
// path, and calls listen(2).  Returns the listening fd.
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

// CompletionNotifier is a callback invoked when a function completes a request.
// Used to notify Prometheus via the gateway's standard HTTPNotifier chain.
type CompletionNotifier func(functionName string, duration time.Duration)
