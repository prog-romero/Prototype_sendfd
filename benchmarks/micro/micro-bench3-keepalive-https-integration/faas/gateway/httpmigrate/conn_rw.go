package httpmigrate

import (
	"net"
	"time"
)

// ConnRW wraps a net.Conn and prepends already-peeked bytes to the Read
// stream.  This allows the HTTP server to see the full request line even
// though those bytes were already consumed by MSG_PEEK in RunLoop.
type ConnRW struct {
	net.Conn
	peeked []byte
	offset int
}

// NewConnRW creates a ConnRW that will replay peeked before delegating
// further reads to inner.
func NewConnRW(inner net.Conn, peeked []byte) *ConnRW {
	b := make([]byte, len(peeked))
	copy(b, peeked)
	return &ConnRW{Conn: inner, peeked: b}
}

// Read first drains the peeked bytes, then falls through to the underlying
// net.Conn.Read.  Once the peeked buffer is exhausted the slice is released.
func (c *ConnRW) Read(b []byte) (int, error) {
	if c.offset < len(c.peeked) {
		n := copy(b, c.peeked[c.offset:])
		c.offset += n
		if c.offset >= len(c.peeked) {
			c.peeked = nil // allow GC
		}
		return n, nil
	}
	return c.Conn.Read(b)
}

// Write delegates directly to the underlying connection.
func (c *ConnRW) Write(b []byte) (int, error) { return c.Conn.Write(b) }

// Close delegates directly.
func (c *ConnRW) Close() error { return c.Conn.Close() }

// LocalAddr delegates directly.
func (c *ConnRW) LocalAddr() net.Addr { return c.Conn.LocalAddr() }

// RemoteAddr delegates directly.
func (c *ConnRW) RemoteAddr() net.Addr { return c.Conn.RemoteAddr() }

// SetDeadline delegates directly.
func (c *ConnRW) SetDeadline(t time.Time) error { return c.Conn.SetDeadline(t) }

// SetReadDeadline delegates directly.
func (c *ConnRW) SetReadDeadline(t time.Time) error { return c.Conn.SetReadDeadline(t) }

// SetWriteDeadline delegates directly.
func (c *ConnRW) SetWriteDeadline(t time.Time) error { return c.Conn.SetWriteDeadline(t) }
