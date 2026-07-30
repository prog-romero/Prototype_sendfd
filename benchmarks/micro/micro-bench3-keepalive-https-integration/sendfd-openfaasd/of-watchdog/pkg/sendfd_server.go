//go:build linux

// Package pkg — sendfd_server.go
//
// StartSendFDServer creates a SOCK_SEQPACKET Unix domain socket at
// <socketDir>/<ownIP>.sock and services the FD-migration messages that the
// faasd PROVIDER forwards to it.
//
// Data path: the gateway never talks to a watchdog directly. It hands the
// client fd (+ a completion-pipe fd) and the httpmigrate payload (target
// function name, and — HTTPS — the exported TLS state) to the provider over
// <socketDir>/provider.sock. The provider resolves the target container's CNI
// IP and forwards the SAME fds + payload to that container's <ip>.sock, i.e.
// THIS socket. FDs cross the process boundary via SCM_RIGHTS; the payload is the
// message body.
//
// The same socket also receives WRONG-OWNER keep-alive connections: when the
// next request on a kept-alive connection targets a different function, the
// current owner sends the connection back to the provider, which re-routes it
// here (to the correct container's <ip>.sock).
//
// SOCK_SEQPACKET is required for SCM_RIGHTS across process boundaries.
//
// Two mutually-exclusive behaviours, selected by the handler argument:
//
//   - Full proxy (handler != nil) — the mode used by the SeBS / BeFaaS
//     functions. The watchdog itself drives the whole connection for its
//     keep-alive lifetime: TLS restore, owner peek, wrong-owner re-route, HTTP
//     framing, wolfSSL read/write (HTTPS) and the direct (encrypted) response
//     write to the client. It only calls handler for the business logic (a
//     reverse-proxy to the plain HTTP function, e.g. gunicorn). See wd_bridge.c
//     / fullproxy*.go.
//         provider  : sendmsg(<ip>.sock, [clientFD, pipeFD], payload)
//         watchdog  : recvmsg → serveFullProxyConn (drives the connection) →
//                     handler.ServeHTTP → encrypted response to clientFD → pipe
//
//   - Legacy relay (handler == nil) — NOT used by the SeBS / BeFaaS functions.
//     The watchdog only forwards each received [clientFD, pipeFD] + payload to a
//     standalone C function worker's socket (<socketDir>/<ownIP>-fn.sock); that
//     worker does all the plumbing itself.
//         provider  : sendmsg(<ip>.sock, [clientFD, pipeFD], payload)
//         watchdog  : recvmsg → sendmsg(<ip>-fn.sock, [clientFD, pipeFD], payload)
//         C worker  : recvmsg → peek/route/parse/respond → signal pipe

package pkg

import (
	"context"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/unix"

	"github.com/openfaas/of-watchdog/config"
)

const kaPayloadSize = 32768 // enough for both HTTP (160B) and HTTPS (~16KB) payloads

// microbenchNowNs returns a monotonic timestamp in nanoseconds
// (CLOCK_MONOTONIC_RAW). It is system-wide, so timestamps taken here (watchdog)
// are directly comparable with those taken in the provider on the same host.
// [MICROBENCH] helper — used only for the migration cost measurements.
func microbenchNowNs() int64 {
	var ts unix.Timespec
	_ = unix.ClockGettime(unix.CLOCK_MONOTONIC_RAW, &ts)
	return int64(ts.Sec)*1_000_000_000 + int64(ts.Nsec)
}

// getContainerIP returns the first non-loopback IPv4 address found on any
// network interface.  This matches the container's CNI address that the
// provider resolves (via containerd) to pick the <ip>.sock to forward to.
func getContainerIP() (string, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "", err
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, addrErr := iface.Addrs()
		if addrErr != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip4 := ip.To4(); ip4 != nil {
				return ip4.String(), nil
			}
		}
	}
	return "", fmt.Errorf("no non-loopback IPv4 address found")
}

// StartSendFDServer is the top-level entry point.  It discovers the container's
// own IP and creates the provider-facing socket at <socketDir>/<ip>.sock (the
// one the provider forwards migrations to).
//
// Behaviour depends on handler:
//
//   - handler == nil (legacy mode): every received pair of FDs is relayed to a
//     standalone C worker's socket at <socketDir>/<ip>-fn.sock, and that worker
//     does all the connection plumbing itself.
//
//   - handler != nil (full-proxy mode): the watchdog itself drives the whole
//     connection (TLS restore, owner peek, wrong-owner re-route, keep-alive,
//     HTTP framing and the direct response write) and invokes handler for the
//     business logic. The function process is a plain HTTP server.
//
// The function blocks until ctx is cancelled or a fatal error occurs.
func StartSendFDServer(ctx context.Context, cfg config.WatchdogConfig, handler http.Handler) {
	socketDir := cfg.SendFDSocketDir

	ip, err := getContainerIP()
	if err != nil {
		log.Printf("[sendfd] getContainerIP: %v\n", err)
		return
	}

	if err := os.MkdirAll(socketDir, 0o777); err != nil {
		log.Printf("[sendfd] MkdirAll %s: %v\n", socketDir, err)
		return
	}

	fullProxy := handler != nil

	// The provider forwards migrations (initial hops AND re-routed wrong-owner
	// keep-alive connections) to this container by its resolved IP, i.e. to
	// <socketDir>/<ip>.sock — the socket created just below. We also publish a
	// by-name alias <socketDir>/<fnName>.sock -> <ip>.sock (compatibility for any
	// by-name routing); the provider itself routes by IP.
	gwSockPath := filepath.Join(socketDir, ip+".sock")
	_ = os.Remove(gwSockPath) // remove stale socket from a previous run

	listenFD, err := createSeqpacketSocket(gwSockPath, 512)
	if err != nil {
		log.Printf("[sendfd] listen %s: %v\n", gwSockPath, err)
		return
	}
	defer syscall.Close(listenFD)
	_ = os.Chmod(gwSockPath, 0o777)

	if fullProxy {
		if err := initFullProxy(cfg); err != nil {
			log.Printf("[sendfd] full-proxy init failed: %v\n", err)
			return
		}
		if cfg.OwnFunctionName != "" {
			nameSock := filepath.Join(socketDir, cfg.OwnFunctionName+".sock")
			_ = os.Remove(nameSock)
			if err := os.Symlink(gwSockPath, nameSock); err != nil {
				log.Printf("[sendfd] symlink %s -> %s: %v\n", nameSock, gwSockPath, err)
			}
		}
		log.Printf("[sendfd] full-proxy listening on %s (fn=%q)\n", gwSockPath, cfg.OwnFunctionName)
	} else {
		log.Printf("[sendfd] listening on %s\n", gwSockPath)
	}

	// Accept loop — ctx cancellation stops the loop via a background goroutine
	// that closes the listening fd.
	go func() {
		<-ctx.Done()
		syscall.Close(listenFD)
	}()

	fnSockPath := filepath.Join(socketDir, ip+"-fn.sock")

	for {
		connFD, _, acceptErr := syscall.Accept(listenFD)
		if acceptErr != nil {
			select {
			case <-ctx.Done():
				return
			default:
			}
			if acceptErr == syscall.EINTR {
				continue
			}
			log.Printf("[sendfd] accept: %v\n", acceptErr)
			return
		}
		if fullProxy {
			go acceptFullProxy(connFD, cfg, handler)
		} else {
			go relayToFunction(connFD, fnSockPath)
		}
	}
}

// acceptFullProxy receives the 2 FDs (clientFD, pipeWriteFD) + payload from the
// provider (an initial migration, or a re-routed wrong-owner keep-alive
// connection) and hands them to the full-proxy driver, which owns the
// connection for its whole keep-alive lifetime.
func acceptFullProxy(connFD int, cfg config.WatchdogConfig, handler http.Handler) {
	defer syscall.Close(connFD)

	payload := make([]byte, kaPayloadSize)
	fd1, fd2, recvErr := recvfdsWithState(connFD, payload)
	if recvErr != nil {
		log.Printf("[sendfd] full-proxy recvfds: %v\n", recvErr)
		return
	}

	// [MICROBENCH] timestamp right after recvmsg completes: this is the watchdog
	// side of the provider->watchdog sendfd hop. Pair with the provider's
	// provider_sendfd_ts (same host, same CLOCK_MONOTONIC_RAW) to get the net
	// sendfd time = watchdog_recvfd_ts - provider_sendfd_ts.
	if microbenchOn {
		log.Printf("[MICROBENCH] watchdog_recvfd_ts=%d\n", microbenchNowNs())
	}

	// serveFullProxyConn takes ownership of fd1 and fd2 (it closes them).
	serveFullProxyConn(fd1, fd2, payload, cfg, handler)
}

// relayToFunction (legacy mode only) receives 2 FDs + payload from the provider
// and forwards them to a standalone C function worker via fnSockPath.
func relayToFunction(connFD int, fnSockPath string) {
	defer syscall.Close(connFD)

	payload := make([]byte, kaPayloadSize)
	fd1, fd2, recvErr := recvfd2WithState(connFD, payload)
	if recvErr != nil {
		log.Printf("[sendfd] recvfd2: %v\n", recvErr)
		return
	}

	targetFn := parseTargetFunction(payload)
	tryPaths := []string{fnSockPath}
	if targetFn != "" {
		tryPaths = append(tryPaths,
			filepath.Join(filepath.Dir(fnSockPath), targetFn+".sock"),
			filepath.Join(filepath.Dir(fnSockPath), targetFn+"-fn.sock"),
		)
	}

	var fnFD int
	var connErr error
	for _, path := range tryPaths {
		fnFD, connErr = connectStreamSocket(path)
		if connErr == nil {
			if path != fnSockPath {
				log.Printf("[sendfd] fallback connected fn.sock %s\n", path)
			}
			break
		}
		log.Printf("[sendfd] connect fn.sock %s: %v\n", path, connErr)
	}

	if connErr != nil {
		_ = syscall.Close(fd1)
		_ = syscall.Close(fd2)
		return
	}
	defer syscall.Close(fnFD)

	if sendErr := sendfd2WithState(fnFD, fd1, fd2, payload); sendErr != nil {
		log.Printf("[sendfd] sendfd2 to fn: %v\n", sendErr)
	}
	// On success fd1 and fd2 are now owned by the function worker.
}

// recvfd2WithState receives exactly 2 FDs via SCM_RIGHTS + payload via iov.
// payloadBuf must be at least kaPayloadSize bytes.
func recvfd2WithState(unixSock int, payloadBuf []byte) (fd1, fd2 int, err error) {
	oob := make([]byte, syscall.CmsgSpace(2*4)) // CMSG_SPACE(2 * sizeof(int))
	n, _, _, _, recvErr := syscall.Recvmsg(unixSock, payloadBuf, oob, 0)
	if recvErr != nil {
		return -1, -1, fmt.Errorf("recvmsg: %w", recvErr)
	}
	if n == 0 {
		return -1, -1, fmt.Errorf("recvmsg: peer closed connection")
	}

	scms, parseErr := syscall.ParseSocketControlMessage(oob)
	if parseErr != nil {
		return -1, -1, fmt.Errorf("ParseSocketControlMessage: %w", parseErr)
	}
	for _, scm := range scms {
		fds, rightsErr := syscall.ParseUnixRights(&scm)
		if rightsErr != nil {
			continue
		}
		if len(fds) >= 2 {
			return fds[0], fds[1], nil
		}
	}
	return -1, -1, fmt.Errorf("recvmsg: expected 2 FDs in control message")
}

// sendfd2WithState sends 2 FDs via SCM_RIGHTS + payload via iov.
// After a successful sendmsg the caller's fd1 and fd2 are closed.
func sendfd2WithState(unixSock, fd1, fd2 int, payload []byte) error {
	defer syscall.Close(fd1)
	defer syscall.Close(fd2)
	rights := syscall.UnixRights(fd1, fd2)
	if err := syscall.Sendmsg(unixSock, payload, rights, nil, 0); err != nil {
		return fmt.Errorf("sendmsg: %w", err)
	}
	return nil
}

// createSeqpacketSocket creates a SOCK_SEQPACKET Unix domain socket, binds it
// to path, and calls listen(2) with the given backlog.
func createSeqpacketSocket(path string, backlog int) (int, error) {
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

// connectStreamSocket connects a SOCK_STREAM Unix domain socket to path.
// Used to connect to the function worker's -fn.sock (which uses SOCK_STREAM).
func connectStreamSocket(path string) (int, error) {
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
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

func parseTargetFunction(payload []byte) string {
	if len(payload) < 160 {
		return ""
	}
	return strings.TrimRight(string(payload[32:160]), "\x00")
}
