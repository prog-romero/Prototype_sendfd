//go:build linux

package httpmigrate

import (
	"fmt"
	"syscall"
)

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


// recvfd1WithState receives exactly 1 FD via SCM_RIGHTS + payload via iov.
func recvfd1WithState(unixSock int, payloadBuf []byte) (fd int, err error) {
	oob := make([]byte, syscall.CmsgSpace(4)) // CMSG_SPACE(sizeof(int))
	n, _, _, _, recvErr := syscall.Recvmsg(unixSock, payloadBuf, oob, 0)
	if recvErr != nil {
		return -1, fmt.Errorf("recvmsg: %w", recvErr)
	}
	if n == 0 {
		return -1, fmt.Errorf("recvmsg: peer closed connection")
	}

	scms, parseErr := syscall.ParseSocketControlMessage(oob)
	if parseErr != nil {
		return -1, fmt.Errorf("ParseSocketControlMessage: %w", parseErr)
	}
	for _, scm := range scms {
		fds, rightsErr := syscall.ParseUnixRights(&scm)
		if rightsErr != nil {
			continue
		}
		if len(fds) >= 1 {
			// Close any extra FDs if sent
			for i := 1; i < len(fds); i++ {
				syscall.Close(fds[i])
			}
			return fds[0], nil
		}
	}
	return -1, fmt.Errorf("recvmsg: expected 1 FD in control message")
}
