package benchhttps

import (
	"bufio"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
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
