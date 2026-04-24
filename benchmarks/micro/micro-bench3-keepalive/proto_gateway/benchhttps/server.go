package benchhttps

import (
	"context"
	"crypto/tls"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"sync/atomic"
	"syscall"
)

type connStateKey struct{}

type benchConnState struct {
	armed  atomic.Uint32
	top1   atomic.Uint64
	cntfrq atomic.Uint64
}

type benchListener struct {
	listener  net.Listener
	tlsConfig *tls.Config
}

type benchConn struct {
	net.Conn
	rawConn net.Conn
	state   *benchConnState
}

func Run(cfg Config, handler http.Handler) error {
	cert, err := tls.LoadX509KeyPair(cfg.CertFile, cfg.KeyFile)
	if err != nil {
		return fmt.Errorf("load x509 key pair: %w", err)
	}

	rawListener, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", cfg.ListenAddr, err)
	}

	listener := &benchListener{
		listener: rawListener,
		tlsConfig: &tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
			NextProtos:   []string{"http/1.1"},
		},
	}

	server := &http.Server{
		Handler: benchMiddleware(handler),
		ConnContext: func(ctx context.Context, conn net.Conn) context.Context {
			if benchConn, ok := conn.(*benchConn); ok {
				return context.WithValue(ctx, connStateKey{}, benchConn.state)
			}
			return ctx
		},
	}

	return server.Serve(listener)
}

func (ln *benchListener) Accept() (net.Conn, error) {
	rawConn, err := ln.listener.Accept()
	if err != nil {
		return nil, err
	}

	tlsConn := tls.Server(rawConn, ln.tlsConfig)
	if err := tlsConn.Handshake(); err != nil {
		_ = rawConn.Close()
		return nil, err
	}

	state := &benchConnState{}
	state.armed.Store(1)

	return &benchConn{
		Conn:    tlsConn,
		rawConn: rawConn,
		state:   state,
	}, nil
}

func (ln *benchListener) Close() error {
	return ln.listener.Close()
}

func (ln *benchListener) Addr() net.Addr {
	return ln.listener.Addr()
}

func (conn *benchConn) Read(p []byte) (int, error) {
	n, err := conn.Conn.Read(p)
	if n > 0 && conn.state.armed.CompareAndSwap(1, 0) {
		conn.state.top1.Store(benchReadCounter())
		conn.state.cntfrq.Store(benchCounterFreq())
	}

	return n, err
}

func benchMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if state, ok := r.Context().Value(connStateKey{}).(*benchConnState); ok && state != nil {
			top1 := state.top1.Load()
			cntfrq := state.cntfrq.Load()
			if top1 > 0 {
				r.Header.Set("X-Bench2-Top1-Rdtsc", strconv.FormatUint(top1, 10))
			}
			if cntfrq > 0 {
				r.Header.Set("X-Bench2-Cntfrq", strconv.FormatUint(cntfrq, 10))
			}

			defer func() {
				state.top1.Store(0)
				state.cntfrq.Store(0)
				state.armed.Store(1)
			}()
		}

		next.ServeHTTP(w, r)
	})
}

func socketFD(conn net.Conn) (int, error) {
	type syscallConn interface {
		SyscallConn() (syscall.RawConn, error)
	}

	sc, ok := conn.(syscallConn)
	if !ok {
		return 0, fmt.Errorf("connection does not support SyscallConn: %T", conn)
	}

	rawConn, err := sc.SyscallConn()
	if err != nil {
		return 0, err
	}

	fd := -1
	if err := rawConn.Control(func(fileDesc uintptr) {
		fd = int(fileDesc)
	}); err != nil {
		return 0, err
	}
	if fd < 0 {
		return 0, fmt.Errorf("invalid socket fd")
	}

	return fd, nil
}