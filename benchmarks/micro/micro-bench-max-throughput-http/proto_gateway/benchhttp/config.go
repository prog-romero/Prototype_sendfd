package benchhttp

import (
	"errors"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
)

type VanillaConfig struct {
	ListenAddr string
	Upstream   string
}

type ProtoConfig struct {
	ListenAddr   string
	SocketDir    string
	RelaySocket  string
	MaxPeekBytes int
}

func MaybeStartVanillaFromEnv() {
	cfg, err := vanillaConfigFromEnv()
	if err != nil {
		log.Printf("[benchhttp-vanilla] disabled: %v", err)
		return
	}

	go func() {
		log.Printf("[benchhttp-vanilla] enabled listen=%s upstream=%s", cfg.ListenAddr, cfg.Upstream)
		if err := RunVanilla(cfg); err != nil {
			log.Printf("[benchhttp-vanilla] stopped: %v", err)
		}
	}()
}

func MaybeStartProtoFromEnv() {
	cfg, err := protoConfigFromEnv()
	if err != nil {
		log.Printf("[benchhttp-ka-proto] disabled: %v", err)
		return
	}

	go func() {
		log.Printf("[benchhttp-ka-proto] enabled listen=%s socket_dir=%s relay=%s",
			cfg.ListenAddr, cfg.SocketDir, cfg.RelaySocket)

		if err := RunProto(cfg); err != nil {
			log.Printf("[benchhttp-ka-proto] stopped: %v", err)
		}
	}()
}

func vanillaConfigFromEnv() (VanillaConfig, error) {
	if strings.TrimSpace(os.Getenv("BENCH3_HTTP_ENABLE")) != "1" {
		return VanillaConfig{}, errors.New("BENCH3_HTTP_ENABLE not set")
	}

	listen := getenv("BENCH3_HTTP_LISTEN", ":8082")
	upstream := getenv("BENCH3_HTTP_UPSTREAM", "127.0.0.1:8080")

	if listen == "" {
		return VanillaConfig{}, errors.New("BENCH3_HTTP_LISTEN empty")
	}
	if upstream == "" {
		return VanillaConfig{}, errors.New("BENCH3_HTTP_UPSTREAM empty")
	}

	return VanillaConfig{
		ListenAddr: listen,
		Upstream:   upstream,
	}, nil
}

func protoConfigFromEnv() (ProtoConfig, error) {
	if strings.TrimSpace(os.Getenv("HTTPMIGRATE_KA_ENABLE")) != "1" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_KA_ENABLE not set")
	}

	listen     := getenv("HTTPMIGRATE_KA_LISTEN", ":8083")
	socketDir  := getenv("HTTPMIGRATE_KA_SOCKET_DIR", "/run/httpmigrate")
	relaySocket := getenv("HTTPMIGRATE_KA_RELAY_SOCKET", "/run/httpmigrate/relay.sock")

	maxPeek := 8192
	if v := strings.TrimSpace(os.Getenv("HTTPMIGRATE_KA_PEEK_BYTES")); v != "" {
		parsed, err := strconv.Atoi(v)
		if err != nil || parsed <= 0 {
			return ProtoConfig{}, fmt.Errorf("invalid HTTPMIGRATE_KA_PEEK_BYTES=%q", v)
		}
		maxPeek = parsed
	}

	if listen == "" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_KA_LISTEN empty")
	}
	if socketDir == "" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_KA_SOCKET_DIR empty")
	}

	return ProtoConfig{
		ListenAddr:   listen,
		SocketDir:    socketDir,
		RelaySocket:  relaySocket,
		MaxPeekBytes: maxPeek,
	}, nil
}

func getenv(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}