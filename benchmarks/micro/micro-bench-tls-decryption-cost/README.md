# Micro Benchmark: TLS Decryption Cost

This benchmark measures the TLS receive/decrypt cost on the Raspberry Pi without socket or kernel I/O in the timed region.

## Goal

For each payload size, the benchmark measures the time between:

- `top1`: just before the client starts `wolfSSL_read()`
- `top2`: after the full plaintext payload has been recovered by the client

The ciphertext is already available in an in-memory transport before `top1` is taken.

## What is included

- TLS record parsing
- authentication/decryption inside wolfSSL
- TLS receive-state updates
- plaintext delivery to the application buffer

## What is excluded

- TCP and kernel socket I/O
- wakeups and scheduling from a real network path
- TLS handshake
- server-side encryption time
- CSV writing and summary computation

## Method

The benchmark runs entirely inside one process on the Pi using wolfSSL custom memory-I/O callbacks.

For each payload size:

1. Create one client/server TLS pair.
2. Complete the handshake outside the timed region.
3. Disable TLS 1.3 session tickets on the server side to avoid post-handshake traffic in the measurement window.
4. For each iteration, have the server encrypt one plaintext payload into the memory transport.
  The benchmark feeds wolfSSL in 16 KiB application chunks so large payloads are emitted as normal TLS-sized records across different wolfSSL builds.
5. Start the timer immediately before the client begins `wolfSSL_read()`.
6. Stop the timer after the client has fully recovered the plaintext payload.

This gives a fair decryption-path benchmark that isolates the TLS receive/decrypt cost much better than a live socket benchmark.

## Defaults

- TLS version: `1.3`
- Default cipher for TLS 1.3: `TLS13-AES128-GCM-SHA256`
- Warmup per payload size: `100`
- Measured iterations per payload size: `1000`
- Payload sizes: `64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, 64 KiB, 128 KiB, 256 KiB, 512 KiB, 1 MiB`
- Timer source: `bench2_rdtsc()` and `bench2_cntfrq()`

## Files

- `bench_tls_decrypt_memio.c`: standalone benchmark executable
- `common/bench2_rdtsc.h`: local copy of the timing helper
- `run_on_pi.sh`: build-and-run helper for the Raspberry Pi

## Build on the Pi

Prerequisites:

- wolfSSL installed under `/usr/local`
- repository available on the Pi
- certificates available under `libtlspeek/certs/`

Build manually:

```bash
cd benchmarks/micro/micro-bench-tls-decryption-cost
make clean all
```

## Run on the Pi

```bash
bash benchmarks/micro/micro-bench-tls-decryption-cost/run_on_pi.sh
```

Useful overrides:

```bash
ITERATIONS=1000
WARMUP=100
PAYLOAD_SIZES=64,256,1024,4096
PAYLOAD_LABELS="64 B, 256 B, 1 KiB, 4 KiB"
TLS_VERSION=1.3
CIPHER=TLS13-AES128-GCM-SHA256
CPU_CORE=2
SERVER_CERT=~/Prototype_sendfd/libtlspeek/certs/server.crt
SERVER_KEY=~/Prototype_sendfd/libtlspeek/certs/server.key
CA_CERT=~/Prototype_sendfd/libtlspeek/certs/ca.crt
```

Example:

```bash
CPU_CORE=2 CIPHER=TLS13-AES128-GCM-SHA256 \
  bash benchmarks/micro/micro-bench-tls-decryption-cost/run_on_pi.sh
```

## CSV outputs

The helper creates two files in `results/`:

- `decrypt_raw_<timestamp>.csv`
- `decrypt_summary_<timestamp>.csv`

### Raw CSV columns

- `payload_size`
- `sample_index`
- `delta_cycles`
- `cntfrq`
- `delta_ns`
- `delta_us`
- `tls_version`
- `cipher`

### Summary CSV columns

- `payload_size`
- `iterations`
- `warmup`
- `min_ns`
- `mean_ns`
- `stddev_ns`
- `p50_ns`
- `p95_ns`
- `p99_ns`
- `max_ns`
- `min_us`
- `mean_us`
- `p50_us`
- `p95_us`
- `p99_us`
- `max_us`
- `tls_version`
- `cipher`

## Interpretation

This benchmark measures the TLS receive/decrypt path, not just the raw cryptographic primitive. It is therefore appropriate as the decryption component of a larger vanilla-path decomposition, but it should not be confused with a full gateway-to-function measurement.
