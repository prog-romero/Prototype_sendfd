package bench2gw

import (
	"bytes"
	"errors"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
)

// Config holds all parameters needed to run the bench2 prototype gateway listener.
type Config struct {
	ListenAddr   string // e.g. ":9444"
	CertFile     string
	KeyFile      string
	SocketDir    string // directory where per-function Unix sockets live
	RelaySocket  string // relay socket for wrong-owner container → gateway → correct container
	MaxPeekBytes int    // progressive peek max, starting at 1024 and doubling (default 8192)
}

// MaybeStartFromEnv reads configuration from environment variables and starts
// the bench2 gateway in a background goroutine.  If the required enable flag
// is not set it does nothing.
func MaybeStartFromEnv() {
	cfg, err := configFromEnv()
	if err != nil {
		log.Printf("[bench2gw] disabled: %v", err)
		return
	}
	go func() {
		log.Printf("[bench2gw] enabled listen=%s socket_dir=%s relay=%s",
			cfg.ListenAddr, cfg.SocketDir, cfg.RelaySocket)
		if err := Run(cfg); err != nil {
			log.Printf("[bench2gw] stopped: %v", err)
		}
	}()
}

func configFromEnv() (Config, error) {
	if strings.TrimSpace(os.Getenv("BENCH2GW_ENABLE")) != "1" {
		return Config{}, errors.New("BENCH2GW_ENABLE not set to 1")
	}

	listen    := getenv("BENCH2GW_LISTEN",       ":9444")
	cert      := getenv("BENCH2GW_CERT",         "/certs/server.crt")
	key       := getenv("BENCH2GW_KEY",          "/certs/server.key")
	socketDir := getenv("BENCH2GW_SOCKET_DIR",   "/run/bench2")
	relay     := getenv("BENCH2GW_RELAY_SOCKET", "/run/bench2/relay.sock")

	maxPeek := 8192
	if v := strings.TrimSpace(os.Getenv("BENCH2GW_PEEK_BYTES")); v != "" {
		p, err := strconv.Atoi(v)
		if err != nil || p <= 0 {
			return Config{}, fmt.Errorf("invalid BENCH2GW_PEEK_BYTES=%q", v)
		}
		maxPeek = p
	}

	if listen == "" || cert == "" || key == "" || socketDir == "" || relay == "" {
		return Config{}, errors.New("bench2gw env config incomplete")
	}
	return Config{
		ListenAddr:   listen,
		CertFile:     cert,
		KeyFile:      key,
		SocketDir:    socketDir,
		RelaySocket:  relay,
		MaxPeekBytes: maxPeek,
	}, nil
}

func getenv(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}

// parseFunctionName extracts the function name from an HTTP/1.1 request line
// of the form "POST /function/<name> HTTP/1.1".
func parseFunctionName(peek []byte) (string, bool) {
	// find the request line
	nl := bytes.IndexByte(peek, '\n')
	if nl < 0 {
		nl = len(peek)
	}
	line := peek[:nl]

	// skip method
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 < 0 {
		return "", false
	}
	rest := line[sp1+1:]

	// find path end
	sp2 := bytes.IndexByte(rest, ' ')
	var path []byte
	if sp2 >= 0 {
		path = rest[:sp2]
	} else {
		path = rest
	}

	prefix := []byte("/function/")
	if !bytes.HasPrefix(path, prefix) {
		return "", false
	}
	name := path[len(prefix):]
	// strip sub-path or query string
	for _, sep := range []byte{'/', '?', '#'} {
		if i := bytes.IndexByte(name, sep); i >= 0 {
			name = name[:i]
		}
	}
	if len(name) == 0 {
		return "", false
	}
	return string(name), true
}
