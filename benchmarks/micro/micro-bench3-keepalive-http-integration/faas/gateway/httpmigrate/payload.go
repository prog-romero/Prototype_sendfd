// Package httpmigrate implements the SCM_RIGHTS keepalive HTTP migration path
// for the OpenFaaS gateway integration benchmark.
//
// Protocol:
//   - Gateway stamps top1_rdtsc (nanoseconds, CLOCK_MONOTONIC_RAW) just before
//     MSG_PEEK on the accepted TCP connection.
//   - Gateway sends [clientFD, pipeWriteFD] via SCM_RIGHTS + httpmigrate_ka_payload_t
//     as regular iov data, in a single sendmsg() call, to the watchdog's UDS.
//   - Watchdog relays the same two FDs + payload to the function's UDS.
//   - Function worker reads top1 from payload, stamps top2 after consuming the
//     full HTTP body, computes delta_ns, writes the JSON response, then writes
//     8 bytes into pipeWriteFD for the gateway's Prometheus notifier.
//   - Relay: function worker peeks the next request, detects a wrong owner,
//     stamps a fresh top1, sets target_function, and sends [clientFD] + payload
//     to <own-ip>-relay.sock.  The gateway relay creates a new pipe and
//     re-dispatches to the new target container.
//
// Timing convention (compatible with micro-bench3-keepalive-http CSV format):
//   - top1_rdtsc and top2_rdtsc are nanoseconds (CLOCK_MONOTONIC_RAW / UnixNano).
//   - cntfrq is always 1_000_000_000 so that delta_ns = top2 − top1.
//   - This avoids a CGo dependency in the gateway (CGO_ENABLED=0 compatible).

package httpmigrate

import (
	"encoding/binary"
)

const (
	// KAMagic is the magic number that identifies a valid httpmigrate keepalive payload.
	KAMagic uint32 = 0x484D4B41 // 'HMKA'

	// KAVersion is the struct version.
	KAVersion uint32 = 2

	// KACntfrq is the fixed "frequency" used when top1/top2 are in nanoseconds.
	KACntfrq uint64 = 1_000_000_000

	// KAPayloadSize is the exact wire size of KAPayload.
	KAPayloadSize = 160

	// targetFunctionLen is the size of the target_function field.
	targetFunctionLen = 128

	// SocketDir is the default directory for all migration UDS sockets.
	SocketDir = "/run/tlsmigrate"
)

// KAPayload is the Go representation of httpmigrate_ka_payload_t (C struct).
//
// Layout (little-endian, matches the C struct exactly on amd64/arm64):
//
//	offset  0 : uint32  magic
//	offset  4 : uint32  version
//	offset  8 : uint64  top1_rdtsc   (nanoseconds in this integration)
//	offset 16 : uint64  cntfrq       (always 1_000_000_000)
//	offset 24 : uint8   top1_set
//	offset 25 : [7]byte _pad
//	offset 32 : [128]byte target_function (null-terminated C string)
//	total  160 bytes
type KAPayload struct {
	Magic          uint32
	Version        uint32
	Top1Rdtsc      uint64
	Cntfrq         uint64
	Top1Set        uint8
	Pad            [7]uint8
	TargetFunction [targetFunctionLen]byte
}

// Marshal serialises p into a 160-byte little-endian wire buffer.
func (p *KAPayload) Marshal() []byte {
	buf := make([]byte, KAPayloadSize)
	binary.LittleEndian.PutUint32(buf[0:4], p.Magic)
	binary.LittleEndian.PutUint32(buf[4:8], p.Version)
	binary.LittleEndian.PutUint64(buf[8:16], p.Top1Rdtsc)
	binary.LittleEndian.PutUint64(buf[16:24], p.Cntfrq)
	buf[24] = p.Top1Set
	copy(buf[25:32], p.Pad[:])
	copy(buf[32:160], p.TargetFunction[:])
	return buf
}

// UnmarshalPayload deserialises a 160-byte little-endian buffer into a KAPayload.
func UnmarshalPayload(data []byte) *KAPayload {
	if len(data) < KAPayloadSize {
		return nil
	}
	p := &KAPayload{}
	p.Magic = binary.LittleEndian.Uint32(data[0:4])
	p.Version = binary.LittleEndian.Uint32(data[4:8])
	p.Top1Rdtsc = binary.LittleEndian.Uint64(data[8:16])
	p.Cntfrq = binary.LittleEndian.Uint64(data[16:24])
	p.Top1Set = data[24]
	copy(p.Pad[:], data[25:32])
	copy(p.TargetFunction[:], data[32:160])
	return p
}

// SetTarget writes a null-terminated function name into TargetFunction.
func (p *KAPayload) SetTarget(name string) {
	for i := range p.TargetFunction {
		p.TargetFunction[i] = 0
	}
	n := copy(p.TargetFunction[:], name)
	if n < targetFunctionLen {
		p.TargetFunction[n] = 0
	}
}

// Target returns the null-terminated function name as a Go string.
func (p *KAPayload) Target() string {
	end := 0
	for end < targetFunctionLen && p.TargetFunction[end] != 0 {
		end++
	}
	return string(p.TargetFunction[:end])
}

// NewPayload builds a fresh KAPayload stamped with the given nanosecond top1.
func NewPayload(top1Ns uint64, targetFn string) *KAPayload {
	p := &KAPayload{
		Magic:     KAMagic,
		Version:   KAVersion,
		Top1Rdtsc: top1Ns,
		Cntfrq:    KACntfrq,
		Top1Set:   1,
	}
	p.SetTarget(targetFn)
	return p
}
