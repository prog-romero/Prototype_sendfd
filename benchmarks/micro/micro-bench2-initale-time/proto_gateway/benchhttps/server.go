package benchhttps

import (
	"bufio"
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
