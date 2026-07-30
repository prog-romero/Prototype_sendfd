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
