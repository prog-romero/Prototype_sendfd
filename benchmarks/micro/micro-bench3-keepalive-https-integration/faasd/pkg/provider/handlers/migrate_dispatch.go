// Copyright (c) OpenFaaS Author(s) 2021. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

package handlers

import (
	"context"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/containerd/containerd"
	"github.com/containerd/containerd/namespaces"
	faasd "github.com/openfaas/faasd/pkg"
	cninetwork "github.com/openfaas/faasd/pkg/cninetwork"
	"golang.org/x/sys/unix"
)

// microbenchNowNs returns a monotonic timestamp in nanoseconds
// (CLOCK_MONOTONIC_RAW). It is system-wide, so timestamps taken here (provider)
// are directly comparable with those taken in the watchdog on the same host.
// [MICROBENCH] helper — used only for the migration cost measurements.
func microbenchNowNs() int64 {
	var ts unix.Timespec
	_ = unix.ClockGettime(unix.CLOCK_MONOTONIC_RAW, &ts)
	return int64(ts.Sec)*1_000_000_000 + int64(ts.Nsec)
}

// migrate_dispatch.go — provider-side FD/TLS-state migration router.
//
// In the provider-routed data path the OpenFaaS gateway no longer talks to a
// function's watchdog directly.  Instead it hands the client FD + TLS state +
// target function name to the provider over a single Unix socket
// (<hostSocketDir>/provider.sock).  The provider resolves the target
// container's CNI IP in-process (containerd + CNI — no HTTP round-trip) and
// forwards the FD(s) + payload to that container's watchdog socket
// (<hostSocketDir>/<ip>.sock), exactly mirroring the normal OpenFaaS request
// path (client → gateway → provider → function).
//
// The same socket also receives wrong-owner keep-alive connections that a
// function worker returns mid-stream: the provider re-routes them to the
// correct container, since it is the component that knows every container's IP.
//
// Path note: faasd runs on the host, so it uses the host directory
// /var/lib/faasd/tlsmigrate, which is bind-mounted into every container at
// /run/tlsmigrate (see MakeDeployHandler).

const (
	// migratePayloadSize bounds one received message.  It must be >= the largest
	// payload sent by the gateway/worker (HTTP: 160 bytes; HTTPS: 160 + TLS
	// serial).  Matches the watchdog's receive buffer so SOCK_SEQPACKET messages
	// are never truncated.
	migratePayloadSize = 32768

	// The null-terminated target function name lives at bytes [32:160] of the
	// wire payload (identical layout to the gateway's KAPayload).
	migrateTargetFnOffset = 32
	migrateTargetFnEnd    = 160

	// migrateIPCacheTTL bounds how long a resolved container IP is cached so a
	// container that restarts with a new CNI address is picked up within this
	// window (mirrors the gateway's former 5-minute IP cache).
	migrateIPCacheTTL = 5 * time.Minute
)

type migrateIPCacheEntry struct {
	ip       string
	cachedAt time.Time
}

var (
	migrateIPMu    sync.RWMutex
	migrateIPCache = map[string]migrateIPCacheEntry{}
)

// StartMigrateDispatcher creates the provider's migration socket at
// <hostSocketDir>/provider.sock (SOCK_SEQPACKET) and services it until ctx is
// cancelled.  Each accepted message is handled in its own goroutine.
func StartMigrateDispatcher(ctx context.Context, client *containerd.Client, hostSocketDir string) {
	if err := os.MkdirAll(hostSocketDir, 0o777); err != nil {
		log.Printf("[migrate] MkdirAll %s: %v\n", hostSocketDir, err)
		return
	}
	sockPath := filepath.Join(hostSocketDir, "provider.sock")
	_ = os.Remove(sockPath) // remove a stale socket from a previous run

	listenFD, err := bindSeqpacket(sockPath, 1024)
	if err != nil {
		log.Printf("[migrate] bind %s: %v\n", sockPath, err)
		return
	}
	_ = os.Chmod(sockPath, 0o777)
	log.Printf("[migrate] provider migration dispatcher listening on %s\n", sockPath)

	// Unblock Accept on shutdown by closing the listening fd.
	go func() {
		<-ctx.Done()
		_ = syscall.Close(listenFD)
	}()

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
			log.Printf("[migrate] accept: %v\n", acceptErr)
			return
		}
		go handleMigrate(connFD, client, hostSocketDir)
	}
}

// handleMigrate receives one migration message (1 or 2 FDs + payload), resolves
// the target container IP, and forwards to that container's watchdog socket.
func handleMigrate(connFD int, client *containerd.Client, hostSocketDir string) {
	defer syscall.Close(connFD)

	buf := make([]byte, migratePayloadSize)
	fds, n, err := recvFDs(connFD, buf)
	if err != nil {
		log.Printf("[migrate] recv: %v\n", err)
		return
	}
	if len(fds) == 0 {
		log.Printf("[migrate] no FDs in control message\n")
		return
	}
	// SCM_RIGHTS duplicates every received FD into this process; we must always
	// close our own copies once forwarded (the watchdog gets its own duplicates).
	defer func() {
		for _, fd := range fds {
			_ = syscall.Close(fd)
		}
	}()

	targetFn := parseMigrateTarget(buf, n)
	if targetFn == "" {
		log.Printf("[migrate] empty target function name in payload\n")
		return
	}

	ip, err := resolveFunctionIP(client, targetFn)
	if err != nil {
		log.Printf("[migrate] resolve IP fn=%s: %v\n", targetFn, err)
		return
	}

	// [MIGRATE-VERIFY] TEMPORARY: confirm, per response, that the request went
	// THROUGH the provider. kind="initial": first hop from the gateway (2 FDs).
	// kind="relay": wrong-owner keep-alive returned by a watchdog (1 FD) that the
	// provider re-routes to the correct container. Remove once verified.
	kind := "initial"
	if len(fds) != 2 {
		kind = "relay"
	}
	log.Printf("[MIGRATE-VERIFY] provider routed fn=%s ip=%s kind=%s\n", targetFn, ip, kind)

	wdPath := filepath.Join(hostSocketDir, ip+".sock")
	wdFD, err := connectSeqpacket(wdPath)
	if err != nil {
		log.Printf("[migrate] connect watchdog %s (fn=%s): %v\n", wdPath, targetFn, err)
		return
	}
	defer syscall.Close(wdFD)

	payload := buf[:n]

	switch len(fds) {
	case 2:
		// Gateway first hop: [clientFD, pipeWriteFD].  Forward both verbatim so
		// the gateway keeps owning the completion pipe (metrics stay in gateway).
		// [MICROBENCH] stamp just before sendmsg: start of provider->watchdog sendfd.
		tSend := microbenchNowNs()
		if err := sendFDs(wdFD, []int{fds[0], fds[1]}, payload); err != nil {
			log.Printf("[migrate] forward (initial) to watchdog fn=%s ip=%s: %v\n", targetFn, ip, err)
		}
		log.Printf("[MICROBENCH] provider_sendfd_ts=%d fn=%s kind=initial\n", tSend, targetFn)

	default:
		// Worker relay of a wrong-owner keep-alive: only [clientFD] arrives.
		// Synthesise a completion pipe so the watchdog/function protocol (always
		// 2 FDs) is satisfied; drain and close the read end here.
		clientFD := fds[0]
		var pipeEnds [2]int
		if err := syscall.Pipe(pipeEnds[:]); err != nil {
			log.Printf("[migrate] pipe (relay) fn=%s: %v\n", targetFn, err)
			return
		}
		pipeR, pipeW := pipeEnds[0], pipeEnds[1]

		// [MICROBENCH] stamp just before sendmsg: start of provider->watchdog sendfd.
		tSend := microbenchNowNs()
		if err := sendFDs(wdFD, []int{clientFD, pipeW}, payload); err != nil {
			log.Printf("[migrate] forward (relay) to watchdog fn=%s ip=%s: %v\n", targetFn, ip, err)
			_ = syscall.Close(pipeR)
			_ = syscall.Close(pipeW)
			return
		}
		log.Printf("[MICROBENCH] provider_sendfd_ts=%d fn=%s kind=relay\n", tSend, targetFn)
		_ = syscall.Close(pipeW) // our copy; the function holds its own duplicate
		go drainAndClosePipe(pipeR)
	}
}

// parseMigrateTarget extracts the null-terminated target function name from the
// payload (bytes [32:160]).
func parseMigrateTarget(buf []byte, n int) string {
	if n < migrateTargetFnEnd {
		return ""
	}
	region := buf[migrateTargetFnOffset:migrateTargetFnEnd]
	end := 0
	for end < len(region) && region[end] != 0 {
		end++
	}
	return string(region[:end])
}

// resolveFunctionIP returns the CNI IP of a running function container, using a
// short-lived cache to avoid hitting containerd on every request.
func resolveFunctionIP(client *containerd.Client, name string) (string, error) {
	migrateIPMu.RLock()
	entry, ok := migrateIPCache[name]
	migrateIPMu.RUnlock()
	if ok && entry.ip != "" && time.Since(entry.cachedAt) < migrateIPCacheTTL {
		return entry.ip, nil
	}

	ctx := namespaces.WithNamespace(context.Background(), faasd.DefaultFunctionNamespace)

	c, err := client.LoadContainer(ctx, name)
	if err != nil {
		return "", fmt.Errorf("load container %s: %w", name, err)
	}
	task, err := c.Task(ctx, nil)
	if err != nil {
		return "", fmt.Errorf("task %s: %w", name, err)
	}
	ip, err := cninetwork.GetIPAddress(name, task.Pid())
	if err != nil {
		return "", fmt.Errorf("get IP %s: %w", name, err)
	}

	migrateIPMu.Lock()
	migrateIPCache[name] = migrateIPCacheEntry{ip: ip, cachedAt: time.Now()}
	migrateIPMu.Unlock()
	return ip, nil
}

// ── low-level Unix socket / SCM_RIGHTS helpers ───────────────────────────────

// recvFDs receives one SOCK_SEQPACKET message: the iov payload (into buf) plus
// every FD carried in the SCM_RIGHTS control message.
func recvFDs(connFD int, buf []byte) (fds []int, n int, err error) {
	oob := make([]byte, syscall.CmsgSpace(2*4)) // room for up to 2 FDs
	n, _, _, _, rerr := syscall.Recvmsg(connFD, buf, oob, 0)
	if rerr != nil {
		return nil, 0, fmt.Errorf("recvmsg: %w", rerr)
	}
	if n == 0 {
		return nil, 0, fmt.Errorf("peer closed connection")
	}
	scms, perr := syscall.ParseSocketControlMessage(oob)
	if perr != nil {
		return nil, 0, fmt.Errorf("parse control message: %w", perr)
	}
	for _, scm := range scms {
		got, rterr := syscall.ParseUnixRights(&scm)
		if rterr != nil {
			continue
		}
		fds = append(fds, got...)
	}
	return fds, n, nil
}

// sendFDs sends payload + the given FDs via SCM_RIGHTS in a single sendmsg.
// It does NOT close the caller's FDs — the caller owns that decision.
func sendFDs(connFD int, fds []int, payload []byte) error {
	rights := syscall.UnixRights(fds...)
	if err := syscall.Sendmsg(connFD, payload, rights, nil, 0); err != nil {
		return fmt.Errorf("sendmsg: %w", err)
	}
	return nil
}

// drainAndClosePipe reads (and discards) the worker's completion timestamp so a
// write to the pipe never raises EPIPE, then closes the read end.
func drainAndClosePipe(fd int) {
	var b [8]byte
	for {
		n, err := syscall.Read(fd, b[:])
		if n == 0 || err != nil {
			break
		}
	}
	_ = syscall.Close(fd)
}

// bindSeqpacket creates, binds and listens a SOCK_SEQPACKET Unix socket.
func bindSeqpacket(path string, backlog int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	_ = os.Remove(path)
	if err := syscall.Bind(fd, &syscall.SockaddrUnix{Name: path}); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("bind %s: %w", path, err)
	}
	if err := syscall.Listen(fd, backlog); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("listen %s: %w", path, err)
	}
	return fd, nil
}

// connectSeqpacket connects a SOCK_SEQPACKET Unix socket to path.
func connectSeqpacket(path string) (int, error) {
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	if err := syscall.Connect(fd, &syscall.SockaddrUnix{Name: path}); err != nil {
		_ = syscall.Close(fd)
		return -1, fmt.Errorf("connect %s: %w", path, err)
	}
	return fd, nil
}
