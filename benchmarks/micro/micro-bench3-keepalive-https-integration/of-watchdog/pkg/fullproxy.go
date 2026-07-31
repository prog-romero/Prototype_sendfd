//go:build linux

// Package pkg — fullproxy.go
//
// Shared (build-tag independent) helpers for the full-proxy path ("Approach 3").
//
// In full-proxy mode the watchdog owns the migrated client connection for its
// whole keep-alive lifetime and performs ALL the plumbing the C function
// workers used to do:
//
//   - HTTP  : raw recv/send on the client FD.
//   - HTTPS : wolfSSL session restore + tls_read_peek + wolfSSL_read/write
//             (implemented in the CGO bridge, see fullproxy_cgo.go).
//
// For every request the connection driver hands the fully-assembled plaintext
// HTTP request to invokeBusinessLogic, which runs it through the watchdog's
// normal requestHandler (HTTP reverse-proxy to the plain HTTP function on
// UpstreamURL) and returns a ready-to-send HTTP/1.1 response.
//
// The function process therefore only implements business logic; it never sees
// a file descriptor, never touches TLS and never relays anything.

package pkg

import (
	"bufio"
	"bytes"
	"fmt"
	"net/http"
	"strings"
	"syscall"
)

// invokeBusinessLogic runs one plaintext HTTP request through handler and
// serialises the response into raw HTTP/1.1 wire bytes.
//
// reqBytes is the full request (request-line + headers + body) exactly as the
// connection driver read it from the client. shouldClose controls the
// Connection header of the produced response so the keep-alive state stays
// consistent between the client and the watchdog.
func invokeBusinessLogic(handler http.Handler, reqBytes []byte, shouldClose bool) []byte {
	// Graceful shutdown: the migrated connection is driven entirely by the
	// full-proxy and no longer transits the gateway, so this is the only place
	// that can tell the client the function is going away. Answer 503 and force
	// the connection closed rather than serve business logic while draining.
	if isShuttingDown() {
		return errorResponseBytes(http.StatusServiceUnavailable, true)
	}

	req, err := http.ReadRequest(bufio.NewReader(bytes.NewReader(reqBytes)))
	if err != nil {
		return errorResponseBytes(http.StatusBadRequest, shouldClose)
	}

	// IMPORTANT: do NOT clear req.RequestURI here.
	//
	// The HTTP-mode handler (executor.HTTPFunctionRunner.Run) builds the upstream
	// URL by appending req.RequestURI to UpstreamURL:
	//     upstreamURL := f.UpstreamURL.String()
	//     if len(r.RequestURI) > 0 { upstreamURL += r.RequestURI }
	// so RequestURI MUST carry the original "/function/<name>/..." path that
	// http.ReadRequest already parsed. Clearing it sent every request to the
	// upstream root "/", which made rpc functions (koa route POST /call) return
	// 404 while only objectrecognition (route POST /) worked.
	//
	// scheme/host are only needed by the stdlib ReverseProxy fallback path
	// (SSE / ndjson / websocket); the Director overrides the host anyway.
	req.URL.Scheme = "http"
	if req.URL.Host == "" {
		req.URL.Host = "function"
	}

	rec := &respRecorder{header: make(http.Header)}
	handler.ServeHTTP(rec, req)
	return rec.serialize(shouldClose)
}

// respRecorder is a minimal in-memory http.ResponseWriter used to capture the
// function's response before it is re-serialised and written to the client FD.
type respRecorder struct {
	header http.Header
	status int
	buf    bytes.Buffer
}

func (r *respRecorder) Header() http.Header { return r.header }

func (r *respRecorder) WriteHeader(status int) {
	if r.status == 0 {
		r.status = status
	}
}

func (r *respRecorder) Write(p []byte) (int, error) {
	if r.status == 0 {
		r.status = http.StatusOK
	}
	return r.buf.Write(p)
}

// serialize renders the captured response as HTTP/1.1 wire bytes. Hop-by-hop
// and length headers are recomputed so the framing is always correct for the
// (possibly re-chunked) body we buffered.
func (r *respRecorder) serialize(shouldClose bool) []byte {
	if r.status == 0 {
		r.status = http.StatusOK
	}

	var b bytes.Buffer
	statusText := http.StatusText(r.status)
	if statusText == "" {
		statusText = "OK"
	}
	fmt.Fprintf(&b, "HTTP/1.1 %d %s\r\n", r.status, statusText)

	for k, vs := range r.header {
		lk := strings.ToLower(k)
		if lk == "content-length" || lk == "connection" || lk == "transfer-encoding" {
			continue
		}
		for _, v := range vs {
			fmt.Fprintf(&b, "%s: %s\r\n", k, v)
		}
	}

	fmt.Fprintf(&b, "Content-Length: %d\r\n", r.buf.Len())
	if shouldClose {
		b.WriteString("Connection: close\r\n")
	} else {
		b.WriteString("Connection: keep-alive\r\n")
	}
	b.WriteString("\r\n")
	b.Write(r.buf.Bytes())
	return b.Bytes()
}

// errorResponseBytes builds a tiny HTTP/1.1 error response used when the request
// could not be parsed at all.
func errorResponseBytes(status int, shouldClose bool) []byte {
	body := fmt.Sprintf("%d %s\n", status, http.StatusText(status))
	var b bytes.Buffer
	fmt.Fprintf(&b, "HTTP/1.1 %d %s\r\n", status, http.StatusText(status))
	b.WriteString("Content-Type: text/plain\r\n")
	fmt.Fprintf(&b, "Content-Length: %d\r\n", len(body))
	if shouldClose {
		b.WriteString("Connection: close\r\n")
	} else {
		b.WriteString("Connection: keep-alive\r\n")
	}
	b.WriteString("\r\n")
	b.WriteString(body)
	return b.Bytes()
}

// recvfdsWithState receives 1 OR 2 FDs via SCM_RIGHTS plus the payload via iov.
//
// The gateway always sends 2 FDs (clientFD, pipeWriteFD). A relayed keep-alive
// connection coming back through the provider socket may carry only the client
// FD (the relaying container signals and closes its own pipe locally, exactly
// like the C workers did). Missing FDs are returned as -1.
func recvfdsWithState(unixSock int, payloadBuf []byte) (fd1, fd2 int, err error) {
	fd1, fd2 = -1, -1
	oob := make([]byte, syscall.CmsgSpace(2*4))
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
		if len(fds) >= 1 {
			fd1 = fds[0]
		}
		if len(fds) >= 2 {
			fd2 = fds[1]
		}
		for i := 2; i < len(fds); i++ {
			_ = syscall.Close(fds[i])
		}
		if fd1 >= 0 {
			return fd1, fd2, nil
		}
	}
	return -1, -1, fmt.Errorf("recvmsg: no FD in control message")
}
