package benchhttp

import (
	"bufio"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"syscall"
)

func RunVanilla(cfg VanillaConfig) error {
	ln, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		return err
	}
	defer ln.Close()

	transport := &http.Transport{
		DisableCompression: true,
		DisableKeepAlives:  false,
	}
	defer transport.CloseIdleConnections()

	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}

		go func(conn net.Conn) {
			if err := serveVanillaConn(conn, cfg.Upstream, transport); err != nil && err != io.EOF {
				log.Printf("[benchhttp-vanilla] serve conn remote=%s: %v", conn.RemoteAddr(), err)
			}
		}(conn)
	}
}

func serveVanillaConn(conn net.Conn, upstream string, transport *http.Transport) error {
	defer conn.Close()

	reader := bufio.NewReader(conn)
	writer := bufio.NewWriter(conn)

	for {
		top1, cntfrq := stampTop1(conn)

		req, err := http.ReadRequest(reader)
		if err != nil {
			return err
		}

		req.URL.Scheme = "http"
		req.URL.Host = upstream
		req.RequestURI = ""
		req.Host = ""

		if top1 > 0 {
			req.Header.Set("X-Bench2-Top1-Rdtsc", strconv.FormatUint(top1, 10))
		}
		if cntfrq > 0 {
			req.Header.Set("X-Bench2-Cntfrq", strconv.FormatUint(cntfrq, 10))
		}

		resp, err := transport.RoundTrip(req)
		if err != nil {
			_ = req.Body.Close()
			return err
		}

		if err := resp.Write(writer); err != nil {
			_ = resp.Body.Close()
			_ = req.Body.Close()
			return err
		}
		if err := writer.Flush(); err != nil {
			_ = resp.Body.Close()
			_ = req.Body.Close()
			return err
		}
		if err := resp.Body.Close(); err != nil {
			_ = req.Body.Close()
			return err
		}
		if err := req.Body.Close(); err != nil {
			return err
		}
		if req.Close || resp.Close {
			return nil
		}
	}
}

func stampTop1(conn net.Conn) (uint64, uint64) {
	fd, err := rawSocketFD(conn)
	if err == nil {
		if benchPendingBytes(fd) == 0 {
			benchWaitReadable(fd)
		}
	}
	return benchReadCounter(), benchCounterFreq()
}

func rawSocketFD(conn net.Conn) (int, error) {
	sysConn, ok := conn.(syscall.Conn)
	if !ok {
		return -1, fmt.Errorf("connection does not expose syscall.Conn")
	}

	rawConn, err := sysConn.SyscallConn()
	if err != nil {
		return -1, err
	}

	fd := -1
	if err := rawConn.Control(func(v uintptr) {
		fd = int(v)
	}); err != nil {
		return -1, err
	}
	if fd < 0 {
		return -1, fmt.Errorf("invalid socket fd")
	}
	return fd, nil
}
