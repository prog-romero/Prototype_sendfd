package benchmodule

import (
    "crypto/tls"
    "encoding/json"
    "fmt"
    "io"
    "net"
    "net/http"
    "runtime"
    "sync"
    "sync/atomic"
    "syscall"
    "time"

    "go.uber.org/zap"
    "golang.org/x/sys/unix"

    "github.com/caddyserver/caddy/v2"
    "github.com/caddyserver/caddy/v2/modules/caddyhttp"
)

const linuxFIONREAD = 0x541B

var benchStatesByRemote sync.Map

func init() {
    caddy.RegisterModule(ListenerWrapper{})
    caddy.RegisterModule(DrainHandler{})
}

type benchConnState struct {
    top1NS atomic.Uint64
    ready  atomic.Bool
}

type ListenerWrapper struct {
    logger *zap.Logger
}

func (ListenerWrapper) CaddyModule() caddy.ModuleInfo {
    return caddy.ModuleInfo{
        ID:  "caddy.listeners.bench_top1",
        New: func() caddy.Module { return new(ListenerWrapper) },
    }
}

func (lw *ListenerWrapper) Provision(ctx caddy.Context) error {
    lw.logger = ctx.Logger()
    return nil
}

func (lw *ListenerWrapper) WrapListener(ln net.Listener) net.Listener {
    return &benchmarkListener{
        Listener: ln,
        logger:   lw.logger,
    }
}

type benchmarkListener struct {
    net.Listener
    logger *zap.Logger
}

func (ln *benchmarkListener) Accept() (net.Conn, error) {
    conn, err := ln.Listener.Accept()
    if err != nil {
        return nil, err
    }

    tlsConn, ok := conn.(*tls.Conn)
    if !ok {
        _ = conn.Close()
        return nil, fmt.Errorf("bench_top1 wrapper expected *tls.Conn, got %T", conn)
    }

    if err := tlsConn.Handshake(); err != nil {
        _ = conn.Close()
        return nil, err
    }

    rawConn, ok := any(tlsConn).(interface{ NetConn() net.Conn })
    if !ok {
        _ = conn.Close()
        return nil, fmt.Errorf("bench_top1 wrapper cannot access underlying net.Conn")
    }

    return &benchmarkConn{
        Conn:    tlsConn,
        tlsConn: tlsConn,
        rawConn: rawConn.NetConn(),
        state:   &benchConnState{},
        connKey: tlsConn.RemoteAddr().String(),
    }, nil
}

type benchmarkConn struct {
    net.Conn
    tlsConn *tls.Conn
    rawConn net.Conn
    state   *benchConnState
    connKey string
}

func (conn *benchmarkConn) Read(p []byte) (int, error) {
    if conn.connKey != "" {
        benchStatesByRemote.Store(conn.connKey, conn.state)
    }

    if !conn.state.ready.Load() {
        if err := waitReadable(conn.rawConn); err != nil {
            return 0, err
        }
        conn.state.top1NS.Store(nowTicks())
        conn.state.ready.Store(true)
    }

    return conn.Conn.Read(p)
}

func (conn *benchmarkConn) ConnectionState() tls.ConnectionState {
    return conn.tlsConn.ConnectionState()
}

func (conn *benchmarkConn) tlsNetConn() net.Conn {
    return conn.tlsConn
}

type DrainHandler struct {
    logger *zap.Logger
    seq    atomic.Uint64
}

func (DrainHandler) CaddyModule() caddy.ModuleInfo {
    return caddy.ModuleInfo{
        ID:  "http.handlers.bench_top2",
        New: func() caddy.Module { return new(DrainHandler) },
    }
}

func (h *DrainHandler) Provision(ctx caddy.Context) error {
    appIface, err := ctx.App("http")
    if err != nil {
        return fmt.Errorf("getting http app: %w", err)
    }

    h.logger = ctx.Logger()

    httpApp, ok := appIface.(*caddyhttp.App)
    if !ok {
        return fmt.Errorf("unexpected http app type: %T", appIface)
    }

    _ = httpApp

    return nil
}

func (h *DrainHandler) ServeHTTP(w http.ResponseWriter, r *http.Request, next caddyhttp.Handler) error {
    _ = next

    stateAny, ok := benchStatesByRemote.LoadAndDelete(r.RemoteAddr)
    if !ok {
        return caddyhttp.Error(http.StatusInternalServerError,
            fmt.Errorf("missing benchmark connection state"))
    }

    state, ok := stateAny.(*benchConnState)
    if !ok || state == nil || !state.ready.Load() {
        return caddyhttp.Error(http.StatusInternalServerError,
            fmt.Errorf("missing benchmark connection state"))
    }

    defer r.Body.Close()

    bytesConsumed, err := io.Copy(io.Discard, r.Body)
    if err != nil {
        return caddyhttp.Error(http.StatusBadRequest, err)
    }

    top2 := nowTicks()
    top1 := state.top1NS.Load()
    requestNo := h.seq.Add(1)

    payload := map[string]any{
        "implementation": "caddy",
        "request_no":     requestNo,
        "top1_rdtsc":     top1,
        "top2_rdtsc":     top2,
        "delta_cycles":   top2 - top1,
        "cntfrq":         uint64(1000000000),
        "delta_ns":       top2 - top1,
        "bytes_expected": r.ContentLength,
        "bytes_consumed": bytesConsumed,
        "tls_version":    tlsVersionName(r.TLS),
        "cipher_suite":   tlsCipherName(r.TLS),
    }

    w.Header().Set("Content-Type", "application/json")
    w.Header().Set("Connection", "close")

    return json.NewEncoder(w).Encode(payload)
}

func nowTicks() uint64 {
    return uint64(time.Now().UnixNano())
}

func tlsVersionName(state *tls.ConnectionState) string {
    if state == nil {
        return "unknown"
    }

    switch state.Version {
    case tls.VersionTLS10:
        return "TLS1.0"
    case tls.VersionTLS11:
        return "TLS1.1"
    case tls.VersionTLS12:
        return "TLS1.2"
    case tls.VersionTLS13:
        return "TLS1.3"
    default:
        return fmt.Sprintf("0x%04x", state.Version)
    }
}

func tlsCipherName(state *tls.ConnectionState) string {
    if state == nil {
        return "unknown"
    }

    return tls.CipherSuiteName(state.CipherSuite)
}

func waitReadable(conn net.Conn) error {
    sysConn, ok := conn.(syscall.Conn)
    if !ok {
        return fmt.Errorf("underlying connection does not support SyscallConn")
    }

    rawConn, err := sysConn.SyscallConn()
    if err != nil {
        return err
    }

    for {
        var avail int
        var ioctlErr error

        if err := rawConn.Control(func(fd uintptr) {
            avail, ioctlErr = unix.IoctlGetInt(int(fd), linuxFIONREAD)
        }); err != nil {
            return err
        }
        if ioctlErr != nil {
            return ioctlErr
        }
        if avail > 0 {
            return nil
        }

        runtime.Gosched()
    }
}

var (
    _ caddy.Provisioner           = (*ListenerWrapper)(nil)
    _ caddy.ListenerWrapper       = (*ListenerWrapper)(nil)
    _ caddy.Provisioner           = (*DrainHandler)(nil)
    _ caddyhttp.MiddlewareHandler = (*DrainHandler)(nil)
)