//go:build linux && cgo

// payload_https.go — HTTPS payload: extends KAPayload with the TLS serial state.
//
// Wire layout sent via sendfd2WithState (same sendmsg path as HTTP):
//
//   [0  .. 159] : KAPayload (160 bytes) — timing + target function name
//   [160 .. N ] : COMPACT tlspeek_serial_t — TLS session state for the worker
//
// The serial is packed to its used length by serial_pack() in tls_listener.c
// (fixed 64-byte header, then length-prefixed tls_blob and http_request), so
// only ~1-2 KB travel instead of the full ~24 KB struct. The watchdog unpacks
// it with serial_unpack() in wd_bridge.c back into a full tlspeek_serial_t.

package httpmigrate

import (
	"fmt"
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
// It embeds KAPayload (160 bytes of timing data) followed by the compact,
// length-packed TLS session state (serial_pack output, typically ~1-2 KB).
type KAPayloadHTTPS struct {
	Base   KAPayload // timing info + target function name
	Serial []byte    // compact tlspeek_serial_t bytes from C (serial_pack)
}

// Marshal serialises the HTTPS payload into a byte slice for sendmsg.
// Layout: KAPayload.Marshal() (160 bytes) || serial bytes.
func (p *KAPayloadHTTPS) Marshal() []byte {
	base := p.Base.Marshal() // 160 bytes
	out := make([]byte, len(base)+len(p.Serial))
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
		p.Base.Top1Set = 1
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
	// Provider-routed data path (HTTPS): identical to HTTP mode — the gateway
	// hands [rawFD, pipeWriteFD] + payload (timing + target fn name + TLS serial)
	// to the faasd provider, which resolves the container IP in-process and
	// forwards to the correct watchdog.  The completion pipe stays owned by the
	// gateway so Prometheus metrics are reported here (option A: unchanged).
	//
	// providerURL is retained for source compatibility but unused: the provider
	// socket path is fixed (ProviderSock) and the provider resolves IPs itself.
	_ = providerURL

	payload.Base.SetTarget(targetFn)

	// Create a completion pipe (writing to pipeW is readable from pipeR; used to
	// signal request completion to the gateway for Prometheus).
	var pipeEnds [2]int
	if err := syscall.Pipe(pipeEnds[:]); err != nil {
		return fmt.Errorf("pipe: %w", err)
	}
	pipeReadFD := pipeEnds[0]
	pipeWriteFD := pipeEnds[1]

	// Connect to the provider's migration socket and send FDs + payload.
	// IMPORTANT: start readCompletionPipe ONLY after sendfd2WithState succeeds, so
	// a failed dispatch (pipeWriteFD closed without a write) cannot leave the
	// completion goroutine waiting pointlessly on an already-EOF pipe.
	providerFD, err := connectUnixSocket(ProviderSock)
	if err != nil {
		_ = syscall.Close(pipeReadFD)
		_ = syscall.Close(pipeWriteFD)
		return fmt.Errorf("connect provider %s: %w", ProviderSock, err)
	}
	defer syscall.Close(providerFD)

	// sendfd2WithState sends [rawFD, pipeWriteFD] via SCM_RIGHTS + payloadBytes and
	// closes both FDs unconditionally (via defer).
	payloadBytes := payload.Marshal()
	if err := sendfd2WithState(providerFD, rawFD, pipeWriteFD, payloadBytes); err != nil {
		_ = syscall.Close(pipeReadFD)
		// pipeWriteFD already closed by sendfd2WithState's deferred Close.
		return fmt.Errorf("sendfd2 to provider: %w", err)
	}

	// Monitor pipe in background — ONLY after successful dispatch.
	start := time.Now()
	if notifier != nil {
		go readCompletionPipe(pipeReadFD, targetFn, start, notifier)
	} else {
		go func() { syscall.Close(pipeReadFD) }()
	}
	return nil
}
