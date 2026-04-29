package benchhttp

/*
#cgo CFLAGS: -I${SRCDIR} -DTLSPEEK_ENABLE_VERBOSE_LOGS
#cgo LDFLAGS: -lpthread

#include "httpmigrate.h"
#include <stdlib.h>
*/
import "C"
import (
	"bytes"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

func RunProto(cfg ProtoConfig) error {
	os.MkdirAll(cfg.SocketDir, 0777)

	addr, err := net.ResolveTCPAddr("tcp", cfg.ListenAddr)
	if err != nil {
		return err
	}

	listenFD, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	if err != nil {
		return err
	}
	defer syscall.Close(listenFD)

	syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)

	var sockaddr syscall.SockaddrInet4
	sockaddr.Port = addr.Port
	copy(sockaddr.Addr[:], addr.IP.To4())

	if err := syscall.Bind(listenFD, &sockaddr); err != nil {
		return err
	}

	if err := syscall.Listen(listenFD, 4096); err != nil {
		return err
	}

	headerBuf := make([]byte, 8192)
	payload := (*C.httpmigrate_payload_t)(C.malloc(C.size_t(C.sizeof_httpmigrate_payload_t)))
	defer C.free(unsafe.Pointer(payload))

	log.Printf("[benchhttp-proto] started zero-copy HTTP gateway on %s", cfg.ListenAddr)

	for {
		var hdrLen C.int
		log.Printf("[benchhttp-proto] calling accept_peek...")
		clientFD := C.httpmigrate_accept_peek(
			C.int(listenFD),
			(*C.uchar)(unsafe.Pointer(&headerBuf[0])),
			C.size_t(len(headerBuf)),
			&hdrLen,
			payload,
		)

		if clientFD < 0 {
			log.Printf("[benchhttp-proto] accept_peek failed or timed out")
			continue
		}

		log.Printf("[benchhttp-proto] accepted clientFD=%d", int(clientFD))

		fnName, ok := parseFunctionName(headerBuf[:int(hdrLen)])
		if !ok {
			log.Printf("[benchhttp-proto] failed to parse function name from peeked headers")
			syscall.Close(int(clientFD))
			continue
		}

		sockPath := filepath.Join(cfg.SocketDir, fnName+".sock")
		cSock := C.CString(sockPath)
		log.Printf("[benchhttp-proto] sending FD to worker at %s", sockPath)
		rc := C.httpmigrate_send_fd(cSock, clientFD, payload, C.size_t(C.sizeof_httpmigrate_payload_t))
		C.free(unsafe.Pointer(cSock))

		if rc != 0 {
			log.Printf("[benchhttp-proto] failed to send FD to worker at %s", sockPath)
		} else {
			log.Printf("[benchhttp-proto] send FD success")
		}
	}
}

func parseFunctionName(peeked []byte) (string, bool) {
	lineEnd := bytes.Index(peeked, []byte("\r\n"))
	if lineEnd < 0 {
		lineEnd = bytes.IndexByte(peeked, '\n')
	}
	if lineEnd < 0 {
		lineEnd = len(peeked)
	}
	line := bytes.TrimSpace(peeked[:lineEnd])
	parts := bytes.SplitN(line, []byte(" "), 3)
	if len(parts) < 2 {
		return "", false
	}
	path := string(parts[1])
	if !strings.HasPrefix(path, "/function/") {
		return "", false
	}
	name := strings.TrimPrefix(path, "/function/")
	// Strip trailing slashes or query params
	if idx := strings.IndexAny(name, "/? "); idx != -1 {
		name = name[:idx]
	}
	return name, name != ""
}
