package httpmigrate

import (
	"fmt"
	"net"
)

// ChanListener is a net.Listener that receives connections through a Go channel.
// It is used by RunLoop to inject raw TCP connections that do NOT take the
// sendfd migrate path into the standard http.Server handler chain.
type ChanListener struct {
	ch     chan net.Conn
	addr   net.Addr
	closed chan struct{}
}

// NewChanListener creates a ChanListener with the given address (used only
// for reporting via Addr()).  buf is the channel buffer size.
func NewChanListener(addr net.Addr, buf int) *ChanListener {
	return &ChanListener{
		ch:     make(chan net.Conn, buf),
		addr:   addr,
		closed: make(chan struct{}),
	}
}

// Push delivers a connection to the listener so that http.Server.Serve() picks
// it up via Accept().  Returns an error if the listener has already been closed.
func (l *ChanListener) Push(conn net.Conn) error {
	select {
	case l.ch <- conn:
		return nil
	case <-l.closed:
		_ = conn.Close()
		return fmt.Errorf("ChanListener: listener is closed")
	}
}

// Accept implements net.Listener.
func (l *ChanListener) Accept() (net.Conn, error) {
	select {
	case conn, ok := <-l.ch:
		if !ok {
			return nil, fmt.Errorf("ChanListener: channel closed")
		}
		return conn, nil
	case <-l.closed:
		return nil, fmt.Errorf("ChanListener: closed")
	}
}

// Close implements net.Listener.
func (l *ChanListener) Close() error {
	select {
	case <-l.closed:
		// already closed
	default:
		close(l.closed)
	}
	return nil
}

// Addr implements net.Listener.
func (l *ChanListener) Addr() net.Addr {
	return l.addr
}
