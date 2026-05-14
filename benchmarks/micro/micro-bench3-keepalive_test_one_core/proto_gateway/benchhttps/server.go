package benchhttps

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"strconv"
)

func Run(cfg Config, _ http.Handler) error {
	wlistener, err := NewWolfSSLListener(cfg.ListenAddr, cfg.CertFile, cfg.KeyFile)
	if err != nil {
		return err
	}
	defer wlistener.Close()

	transport := &http.Transport{
		DisableCompression: true,
		DisableKeepAlives:  false,
	}
	defer transport.CloseIdleConnections()

	for {
		conn, err := wlistener.Accept()
		if err != nil {
			return err
		}

		go func(conn net.Conn) {
			if err := serveConn(conn, cfg.Upstream, transport); err != nil && err != io.EOF {
				log.Printf("[benchhttps] serve conn remote=%s: %v", conn.RemoteAddr(), err)
			}
		}(conn)
	}
}

// stampingReader wraps req.Body. It passes every Read() through unchanged
// (streaming to the upstream is completely unmodified). When the underlying
// reader signals io.EOF (last TLS record fully decrypted by wolfSSL), it
// atomically records top1_decrypted via benchReadCounter().
type stampingReader struct {
	inner    io.ReadCloser
	stamp    *uint64 // written once on EOF
	cntfrq   uint64  // carried from top1 stamp
}

func (s *stampingReader) Read(p []byte) (int, error) {
	n, err := s.inner.Read(p)
	if n > 0 {
		// Update on every real read so the final update captures the moment
		// the last TLS byte was decrypted, regardless of whether the transport
		// issues a trailing zero-byte EOF read.
		*s.stamp = benchReadCounter()
	} else if err == io.EOF && *s.stamp == 0 {
		// Fallback: empty-body EOF.
		*s.stamp = benchReadCounter()
	}
	return n, err
}

func (s *stampingReader) Close() error { return s.inner.Close() }

func serveConn(conn net.Conn, upstream string, transport *http.Transport) error {
	defer conn.Close()

	reader := bufio.NewReader(conn)
	writer := bufio.NewWriter(conn)

	for {
		top1, cntfrq := stampTop1(conn)

		req, err := http.ReadRequest(reader)
		if err != nil {
			return err
		}

		parsedURL, err := url.ParseRequestURI(req.RequestURI)
		if err != nil {
			_ = req.Body.Close()
			return err
		}

		req.URL = parsedURL
		req.URL.Scheme = "http"
		req.URL.Host = upstream
		req.RequestURI = ""
		req.Host = upstream

		if top1 > 0 {
			req.Header.Set("X-Bench2-Top1-Rdtsc", strconv.FormatUint(top1, 10))
		}
		if cntfrq > 0 {
			req.Header.Set("X-Bench2-Cntfrq", strconv.FormatUint(cntfrq, 10))
		}

		// Wrap body with stampingReader — records top1_decrypted on last EOF
		// without buffering or changing streaming behaviour.
		var top1Decrypted uint64
		if req.Body != nil && req.Body != http.NoBody {
			req.Body = &stampingReader{
				inner:  req.Body,
				stamp:  &top1Decrypted,
				cntfrq: cntfrq,
			}
		}

		resp, err := transport.RoundTrip(req)
		if err != nil {
			_ = req.Body.Close()
			return err
		}
		// Fallback: if the body was nil/empty or the transport didn't trigger a
		// final Read, stamp now. By the time RoundTrip returns the full request
		// body has been sent (and thus decrypted). This adds a small response-
		// header-receive overhead, but is far better than reporting 0.
		if top1Decrypted == 0 {
			top1Decrypted = benchReadCounter()
		}

		// Patch the JSON response: inject top1_decrypted_rdtsc, delta_decrypt_ns,
		// and delta_migration_ns. Worker's top_container_before_read is preserved.
		resp, err = patchVanillaResponse(resp, top1, top1Decrypted, cntfrq)
		if err != nil {
			_ = resp.Body.Close()
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

// patchVanillaResponse reads the JSON body from the upstream response,
// injects top1_decrypted_rdtsc, delta_decrypt_ns, and
// top_container_before_read fields, then returns a new *http.Response with the
// patched body. The original response body is consumed and closed.
func patchVanillaResponse(resp *http.Response, top1, top1Decrypted, cntfrq uint64) (*http.Response, error) {
	body, err := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if err != nil {
		resp.Body = io.NopCloser(bytes.NewReader(body))
		return resp, nil // best-effort: return as-is
	}

	var m map[string]interface{}
	if err := json.Unmarshal(bytes.TrimSpace(body), &m); err != nil {
		// Not valid JSON — pass through unchanged
		resp.Body = io.NopCloser(bytes.NewReader(body))
		resp.ContentLength = int64(len(body))
		return resp, nil
	}

	var deltaDecryptNs uint64
	if top1Decrypted > top1 {
		deltaDecryptNs = top1Decrypted - top1
	}

	// Worker already set top_container_before_read (epoll stamp in the container).
	// Read it so we can compute the forwarding latency.
	var workerTopCbr uint64
	if v, ok := m["top_container_before_read"]; ok {
		if f, ok2 := v.(float64); ok2 {
			workerTopCbr = uint64(f)
		}
	}

	// delta_migration_ns (vanilla): time from first TLS byte at gateway (top1)
	// to worker ready to read — directly comparable to proto's sendfd overhead.
	var deltaMigrationNs uint64
	if workerTopCbr > top1 {
		deltaMigrationNs = workerTopCbr - top1
	}

	m["top1_decrypted_rdtsc"] = top1Decrypted
	m["delta_decrypt_ns"] = deltaDecryptNs
	m["delta_migration_ns"] = deltaMigrationNs
	// top_container_before_read is intentionally left as the worker set it.

	patched, err := json.Marshal(m)
	if err != nil {
		resp.Body = io.NopCloser(bytes.NewReader(body))
		resp.ContentLength = int64(len(body))
		return resp, nil
	}
	patched = append(patched, '\n')

	resp.Body = io.NopCloser(bytes.NewReader(patched))
	resp.ContentLength = int64(len(patched))
	return resp, nil
}

func stampTop1(conn net.Conn) (uint64, uint64) {
	if fdConn, ok := conn.(interface {
		RawFD() int
		Pending() int
	}); ok {
		if fdConn.Pending() == 0 {
			/*
			 * wolfSSL can read ahead at the record layer, so the raw socket may
			 * appear empty even though the next HTTP request is already buffered
			 * internally. Wait only briefly for raw readability, then stamp
			 * immediately before the first consuming read.
			 */
			benchWaitReadableTimeout(fdConn.RawFD(), 5)
		}
	}

	return benchReadCounter(), benchCounterFreq()
}
