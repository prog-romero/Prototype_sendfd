package benchhttp

import (
	"io"
	"log"
	"net"
)

func RunVanilla(cfg VanillaConfig) error {
	ln, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		return err
	}
	defer ln.Close()

	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}

		go func(conn net.Conn) {
			if err := serveVanillaConn(conn, cfg.Upstream); err != nil && err != io.EOF {
				log.Printf("[benchhttp-vanilla] serve conn remote=%s: %v", conn.RemoteAddr(), err)
			}
		}(conn)
	}
}

func serveVanillaConn(conn net.Conn, upstream string) error {
	defer conn.Close()

	upConn, err := net.Dial("tcp", upstream)
	if err != nil {
		return err
	}
	defer upConn.Close()

	errCh := make(chan error, 1)

	go func() {
		_, copyErr := io.Copy(upConn, conn)
		if tcp, ok := upConn.(*net.TCPConn); ok {
			_ = tcp.CloseWrite()
		}
		errCh <- copyErr
	}()

	_, downErr := io.Copy(conn, upConn)
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.CloseWrite()
	}

	upErr := <-errCh
	if downErr != nil && downErr != io.EOF {
		return downErr
	}
	if upErr != nil && upErr != io.EOF {
		return upErr
	}
	return nil
}
