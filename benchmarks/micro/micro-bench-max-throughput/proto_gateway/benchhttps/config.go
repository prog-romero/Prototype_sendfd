package benchhttps

import (
	"errors"
	"log"
	"net/http"
	"os"
	"strings"
)

type Config struct {
	ListenAddr string
	CertFile   string
	KeyFile    string
	Upstream   string
}

func MaybeStartFromEnv(handler http.Handler) {
	cfg, err := configFromEnv()
	if err != nil {
		log.Printf("[benchhttps] disabled: %v", err)
		return
	}

	go func() {
		log.Printf("[benchhttps] enabled listen=%s", cfg.ListenAddr)
		if err := Run(cfg, handler); err != nil {
			log.Printf("[benchhttps] stopped: %v", err)
		}
	}()
}

func configFromEnv() (Config, error) {
	if strings.TrimSpace(os.Getenv("BENCH2_TLS_ENABLE")) != "1" {
		return Config{}, errors.New("BENCH2_TLS_ENABLE not set")
	}

	listen := getenv("BENCH2_TLS_LISTEN", ":8444")
	cert := getenv("BENCH2_TLS_CERT", "/certs/server.crt")
	key := getenv("BENCH2_TLS_KEY", "/certs/server.key")
	upstream := getenv("BENCH2_TLS_UPSTREAM", "127.0.0.1:8080")

	if listen == "" {
		return Config{}, errors.New("BENCH2_TLS_LISTEN empty")
	}
	if cert == "" || key == "" || upstream == "" {
		return Config{}, errors.New("BENCH2_TLS_CERT/BENCH2_TLS_KEY/BENCH2_TLS_UPSTREAM empty")
	}

	return Config{
		ListenAddr: listen,
		CertFile:   cert,
		KeyFile:    key,
		Upstream:   upstream,
	}, nil
}

func getenv(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}
