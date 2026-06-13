//go:build linux && cgo

// payload_https.go — HTTPS payload: extends KAPayload with the TLS serial state.
//
// Wire layout sent via sendfd2WithState (same sendmsg path as HTTP):
//
//   [0  .. 159] : KAPayload (160 bytes) — timing + target function name
//   [160 .. N ] : raw tlspeek_serial_t bytes — TLS session state for worker
//
// The worker reads this as: httpmigrate_ka_payload_https_t
// (base 160-byte header, then the serial blob)

package httpmigrate

import (
	"fmt"
	"log"
	"path/filepath"
	"syscall"
	"time"
)

const (
	// KAPayloadHTTPSMagic identifies an HTTPS migration payload.
	KAPayloadHTTPSMagic uint32 = 0x484D4B53 // 'HMKS'

	// KAPayloadHTTPSVersion is the struct version for HTTPS payloads.
	KAPayloadHTTPSVersion uint32 = 1
)

// KAPayloadHTTPS is the Go representation of the HTTPS wire payload.
// It embeds KAPayload (160 bytes of timing data) followed by
// the raw serialised TLS session state (tlspeek_serial_t, ~24 KB).
type KAPayloadHTTPS struct {
	Base   KAPayload // timing info + target function name
	Serial []byte    // raw tlspeek_serial_t bytes from C
}

// Marshal serialises the HTTPS payload into a byte slice for sendmsg.
// Layout: KAPayload.Marshal() (160 bytes) || serial bytes.
func (p *KAPayloadHTTPS) Marshal() []byte {
	base := p.Base.Marshal() // 160 bytes
	out  := make([]byte, len(base)+len(p.Serial))
	copy(out, base)
	copy(out[len(base):], p.Serial)
	return out
}

// NewPayloadHTTPS builds a KAPayloadHTTPS stamped with the given nanosecond top1,
// target function name, and the serialised TLS session bytes from C.
//
// top1Ns     : timestamp (nanoseconds) from getMonotonicNs(); 0 in SUM_PROD mode.
// targetFn   : function name (e.g. "timing-fn-a").
// serialBytes: raw tlspeek_serial_t bytes returned by PeekAndExport().
func NewPayloadHTTPS(top1Ns uint64, targetFn string, serialBytes []byte) *KAPayloadHTTPS {
	p := &KAPayloadHTTPS{
		Base: KAPayload{
			Magic:   KAPayloadHTTPSMagic,
			Version: KAPayloadHTTPSVersion,
			Cntfrq:  KACntfrq,
		},
		Serial: serialBytes,
	}
	if top1Ns > 0 {
		p.Base.Top1Rdtsc = top1Ns
		p.Base.Top1Set   = 1
	}
	p.Base.SetTarget(targetFn)
	return p
}

// ── dispatchMigrateHTTPS ─────────────────────────────────────────────────────

// dispatchMigrateHTTPS is the HTTPS equivalent of dispatchMigrate in relay_server.go.
// Sends (rawFD + payload) to the of-watchdog socket for targetFn.
// The pipe (Unix pipe — one-way communication channel between two FDs) mechanism
// for Prometheus completion notification is identical to HTTP mode.
func dispatchMigrateHTTPS(
	rawFD int,
	targetFn string,
	payload *KAPayloadHTTPS,
	providerURL string,
	notifier CompletionNotifier,
) error {
	// 1. Resolve container IP — same helper as HTTP mode.
	ip, err := ResolveContainerIP(targetFn, providerURL)
	if err != nil {
		return fmt.Errorf("resolve IP for %s: %w", targetFn, err)
	}

	// 2. Ensure relay socket for this container is registered.
	EnsureRelaySocketHTTPS(ip, len(payload.Serial), providerURL, notifier)

	// 3. Create a completion pipe (pipe — kernel mechanism where writing to pipeW
	//    is readable from pipeR, used to signal request completion to gateway).
	var pipeEnds [2]int
	if err := syscall.Pipe(pipeEnds[:]); err != nil {
		return fmt.Errorf("pipe: %w", err)
	}
	pipeReadFD  := pipeEnds[0]
	pipeWriteFD := pipeEnds[1]

	// 4. Connect to of-watchdog's Unix domain socket and send FDs + payload.
	//    IMPORTANT: start readCompletionPipe goroutine ONLY after sendfd2WithState
	//    succeeds.  If dispatch fails, pipeWriteFD is closed (by sendfd2WithState's
	//    deferred Close or the explicit close below) without a write.  The old code
	//    started the goroutine first — readFull then saw (0, nil) from syscall.Read
	//    and spun forever, accumulating spinning goroutines that persisted after
	//    the client disconnected.
	sockPath := filepath.Join(SocketDir, ip+".sock")
	watchdogFD, err := connectUnixSocket(sockPath)
	if err != nil {
		_ = syscall.Close(pipeReadFD)
		_ = syscall.Close(pipeWriteFD)
		return fmt.Errorf("connect watchdog %s: %w", sockPath, err)
	}
	defer syscall.Close(watchdogFD)

	// 5. sendfd2WithState: sends [rawFD, pipeWriteFD] via SCM_RIGHTS + payloadBytes.
	//    sendfd2WithState closes both rawFD and pipeWriteFD unconditionally (via defer).
	payloadBytes := payload.Marshal()
	if err := sendfd2WithState(watchdogFD, rawFD, pipeWriteFD, payloadBytes); err != nil {
		_ = syscall.Close(pipeReadFD)
		// pipeWriteFD already closed by sendfd2WithState's deferred Close.
		return fmt.Errorf("sendfd2 to watchdog: %w", err)
	}

	// 6. Monitor pipe in background — ONLY after successful dispatch.
	start := time.Now()
	if notifier != nil {
		go readCompletionPipe(pipeReadFD, targetFn, start, notifier)
	} else {
		go func() { syscall.Close(pipeReadFD) }()
	}
	return nil
}

// relayListenLoopHTTPS is started in a goroutine by RunLoopHTTPS.
// Workers return wrong-owner keep-alive connections here with the updated TLS serial.
// The relay re-dispatches to the correct container using dispatchMigrateHTTPS.
//
// Note: the relay socket path is per-container IP, created by EnsureRelaySocket.
// This function is the HTTPS-aware version — it handles the larger payload buffer.
func relayListenLoopHTTPS(listenFD int, serialSize int, providerURL string, notifier CompletionNotifier) {
	defer syscall.Close(listenFD)

	// Buffer size for the full HTTPS payload: 160 bytes (KAPayload base) + serial.
	bufSize := KAPayloadSize + serialSize

	for {
		connFD, _, err := syscall.Accept(listenFD)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			log.Printf("[relay-https] accept error: %v\n", err)
			return
		}

		go func(cFD int) {
			defer syscall.Close(cFD)

			// Allocate a dedicated buffer per connection to prevent data races
			buf := make([]byte, bufSize)
			clientFD, relayErr := recvfd1WithState(cFD, buf)
			if relayErr != nil {
				log.Printf("[relay-https] recvfd1 error: %v\n", relayErr)
				return
			}

			// First 160 bytes are the KAPayload base.
			basePay := UnmarshalPayload(buf[:KAPayloadSize])
			if basePay == nil || (basePay.Magic != KAPayloadHTTPSMagic && basePay.Magic != KAMagic) {
				log.Printf("[relay-https] bad magic\n")
				_ = syscall.Close(clientFD)
				return
			}

			targetFn := basePay.Target()
			if targetFn == "" {
				log.Printf("[relay-https] empty target function\n")
				_ = syscall.Close(clientFD)
				return
			}

			// Rebuild payload with serial bytes from relay buffer.
			serial := make([]byte, serialSize)
			if len(buf) >= KAPayloadSize+serialSize {
				copy(serial, buf[KAPayloadSize:KAPayloadSize+serialSize])
			}

			httpsPayload := &KAPayloadHTTPS{Base: *basePay, Serial: serial}

			if err := dispatchMigrateHTTPS(clientFD, targetFn, httpsPayload,
				providerURL, notifier); err != nil {
				log.Printf("[relay-https] dispatch failed fn=%s: %v\n", targetFn, err)
				// Only close clientFD on failure: on success sendfd2WithState
				// (inside dispatchMigrateHTTPS) already closed it via defer.
				// Closing it again would double-close a potentially reused fd.
				_ = syscall.Close(clientFD)
			}
		}(connFD)
	}
}
