package tlsmigratekeepalive

import (
	"bytes"
	"errors"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
)

type Config struct {
	ListenAddr   string
	CertFile     string
	KeyFile      string
	SocketDir    string
	RelaySocket  string
	MaxPeekBytes int
}

func MaybeStartFromEnv() {
	cfg, err := configFromEnv()
	if err != nil {
		log.Printf("[tlsmigrate-keepalive] disabled: %v", err)
		return
	}

	go func() {
		log.Printf("[tlsmigrate-keepalive] enabled listen=%s socket_dir=%s relay=%s",
			cfg.ListenAddr, cfg.SocketDir, cfg.RelaySocket)
		if err := Run(cfg); err != nil {
			log.Printf("[tlsmigrate-keepalive] stopped: %v", err)
		}
	}()
}

func configFromEnv() (Config, error) {
	listen := getenv("TLSMIGRATE_KEEPALIVE_LISTEN", ":9444")
	cert := getenv("TLSMIGRATE_KEEPALIVE_CERT", "/certs/server.crt")
	key := getenv("TLSMIGRATE_KEEPALIVE_KEY", "/certs/server.key")
	socketDir := getenv("TLSMIGRATE_KEEPALIVE_SOCKET_DIR", "/run/tlsmigrate")
	relay := getenv("TLSMIGRATE_KEEPALIVE_RELAY_SOCKET", "/run/tlsmigrate/keepalive-relay.sock")

	maxPeek := 8192
	if v := strings.TrimSpace(os.Getenv("TLSMIGRATE_KEEPALIVE_PEEK_BYTES")); v != "" {
		parsed, err := strconv.Atoi(v)
		if err != nil || parsed <= 0 {
			return Config{}, fmt.Errorf("invalid TLSMIGRATE_KEEPALIVE_PEEK_BYTES=%q", v)
		}
		maxPeek = parsed
	}

	if strings.TrimSpace(os.Getenv("TLSMIGRATE_KEEPALIVE_ENABLE")) != "1" {
		return Config{}, errors.New("TLSMIGRATE_KEEPALIVE_ENABLE not set")
	}
	if listen == "" || cert == "" || key == "" || socketDir == "" || relay == "" {
		return Config{}, errors.New("keepalive env config is incomplete")
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

func parseFunctionName(peeked []byte) (string, bool) {
	lineEnd := bytes.Index(peeked, []byte("\r\n"))
	if lineEnd < 0 {
		lineEnd = bytes.IndexByte(peeked, '\n')
	}
	if lineEnd < 0 {
		lineEnd = len(peeked)
	}
	line := bytes.TrimSpace(peeked[:lineEnd])
	parts := bytes.SplitN(line, []byte(" "), 3)
	if len(parts) < 2 {
		return "", false
	}
	path := string(parts[1])
	if !strings.HasPrefix(path, "/function/") {
		return "", false
	}
	rest := strings.TrimPrefix(path, "/function/")
	end := len(rest)
	for i := 0; i < len(rest); i++ {
		switch rest[i] {
		case '/', '?', ' ':
			end = i
			i = len(rest)
		}
	}
	name := rest[:end]
	if name == "" {
		return "", false
	}
	for _, ch := range name {
		if (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') {
			continue
		}
		switch ch {
		case '-', '_', '.':
			continue
		default:
			return "", false
		}
	}
	return name, true
}