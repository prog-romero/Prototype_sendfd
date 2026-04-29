package benchhttp

import (
	"errors"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"

	"runtime"
	"golang.org/x/sys/unix"
)

type VanillaConfig struct {
	ListenAddr string
	Upstream   string
}

type ProtoConfig struct {
	ListenAddr   string
	SocketDir    string
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
		log.Printf("[benchhttp-proto] disabled: %v", err)
		return
	}

	go func() {
		log.Printf("[benchhttp-proto] enabled listen=%s socket_dir=%s", cfg.ListenAddr, cfg.SocketDir)

		// 🔥 Pin this goroutine to a single OS thread and CPU core
		runtime.LockOSThread()

		var cpuSet unix.CPUSet
		cpuSet.Set(0) // bind to CPU core 0

		if err := unix.SchedSetaffinity(0, &cpuSet); err != nil {
			log.Printf("[benchhttp-proto] WARNING: failed to pin CPU: %v", err)
		} else {
			log.Printf("[benchhttp-proto] pinned to CPU 0")
		}

		if err := RunProto(cfg); err != nil {
			log.Printf("[benchhttp-proto] stopped: %v", err)
		}
	}()
}

func vanillaConfigFromEnv() (VanillaConfig, error) {
	if strings.TrimSpace(os.Getenv("BENCH2_HTTP_ENABLE")) != "1" {
		return VanillaConfig{}, errors.New("BENCH2_HTTP_ENABLE not set")
	}

	listen := getenv("BENCH2_HTTP_LISTEN", ":8082")
	upstream := getenv("BENCH2_HTTP_UPSTREAM", "127.0.0.1:8080")

	if listen == "" {
		return VanillaConfig{}, errors.New("BENCH2_HTTP_LISTEN empty")
	}
	if upstream == "" {
		return VanillaConfig{}, errors.New("BENCH2_HTTP_UPSTREAM empty")
	}

	return VanillaConfig{
		ListenAddr: listen,
		Upstream:   upstream,
	}, nil
}

func protoConfigFromEnv() (ProtoConfig, error) {
	if strings.TrimSpace(os.Getenv("HTTPMIGRATE_ENABLE")) != "1" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_ENABLE not set")
	}

	listen := getenv("HTTPMIGRATE_LISTEN", ":8083")
	socketDir := getenv("HTTPMIGRATE_SOCKET_DIR", "/run/httpmigrate")

	maxPeek := 8192
	if v := strings.TrimSpace(os.Getenv("HTTPMIGRATE_PEEK_BYTES")); v != "" {
		parsed, err := strconv.Atoi(v)
		if err != nil || parsed <= 0 {
			return ProtoConfig{}, fmt.Errorf("invalid HTTPMIGRATE_PEEK_BYTES=%q", v)
		}
		maxPeek = parsed
	}

	if listen == "" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_LISTEN empty")
	}
	if socketDir == "" {
		return ProtoConfig{}, errors.New("HTTPMIGRATE_SOCKET_DIR empty")
	}

	return ProtoConfig{
		ListenAddr:   listen,
		SocketDir:    socketDir,
		MaxPeekBytes: maxPeek,
	}, nil
}

func getenv(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}