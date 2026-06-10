//go:build linux

// Package pkg — sendfd_server.go
//
// StartSendFDServer creates a Unix domain socket at <socketDir>/<ownIP>.sock
// and relays every received pair of FDs + httpmigrate payload to the function
// worker's socket at <socketDir>/<ownIP>-fn.sock.
//
// Socket type: SOCK_SEQPACKET (required for SCM_RIGHTS across process boundaries).
//
// Protocol in this direction (gateway → watchdog → function):
//   - Gateway: sendmsg(watchdog.sock, [clientFD, pipeWriteFD], payload)
//   - Watchdog: recvmsg → sendmsg(fn.sock, [clientFD, pipeWriteFD], payload)
//   - Function: recvmsg → process request → write(pipeWriteFD, ts, 8); close(pipeWriteFD)

package pkg

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

const kaPayloadSize = 160 // sizeof(httpmigrate_ka_payload_t)

// getContainerIP returns the first non-loopback IPv4 address found on any
// network interface.  This matches what the gateway sees as the CNI address.
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
// own IP, creates the gateway-facing socket at <socketDir>/<ip>.sock, and
// accepts connections, relaying each received pair of FDs to the function
// worker's socket at <socketDir>/<ip>-fn.sock.
//
// The function blocks until ctx is cancelled or a fatal error occurs.
func StartSendFDServer(ctx context.Context, socketDir string) {
	ip, err := getContainerIP()
	if err != nil {
		log.Printf("[sendfd] getContainerIP: %v\n", err)
		return
	}

	if err := os.MkdirAll(socketDir, 0o777); err != nil {
		log.Printf("[sendfd] MkdirAll %s: %v\n", socketDir, err)
		return
	}

	gwSockPath := filepath.Join(socketDir, ip+".sock")
	_ = os.Remove(gwSockPath) // remove stale socket from a previous run

	listenFD, err := createSeqpacketSocket(gwSockPath, 512)
	if err != nil {
		log.Printf("[sendfd] listen %s: %v\n", gwSockPath, err)
		return
	}
	defer syscall.Close(listenFD)
	_ = os.Chmod(gwSockPath, 0o777)

	log.Printf("[sendfd] listening on %s\n", gwSockPath)

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
		go relayToFunction(connFD, fnSockPath)
	}
}

// relayToFunction receives 2 FDs + payload from the gateway and forwards them
// to the function worker via fnSockPath.
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

// connectSeqpacketSocket connects a SOCK_SEQPACKET Unix domain socket to path.
func connectSeqpacketSocket(path string) (int, error) {
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
