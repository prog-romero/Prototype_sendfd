# Micro Benchmark: wolfSSL vs Caddy Request Decryption

This benchmark compares the server-side request read/decrypt cost of:

- wolfSSL in a dedicated C benchmark server
- the real Caddy server, instrumented internally

The benchmark runs on the local machine. The client and the measured server
run on the same host.

## Goal

For each payload size, the client sends 100 HTTPS requests, each on a fresh TLS
connection.

For each measured request, we record a server-side interval:

- `top1`: request bytes are already available on the socket and the server is
  just about to begin the first real consuming read for that request
- `top2`: the full HTTP request body for that payload size has been read and
  decrypted by the server stack

The comparison target is therefore:

- wolfSSL read/decrypt path
- Caddy inbound TLS+HTTP read/decrypt path

## Fairness Contract

- One measured HTTP request per TLS connection
- TLS handshake completes before the timed region begins
- HTTP/1.1 only on both sides
- Fixed `Content-Length` body, no chunked transfer
- No compression
- No `Expect: 100-continue`
- No response generation or response write time in the timed region
- No upstream reverse proxying in the timed region for the Caddy path

The benchmark should exclude idle waiting on an empty socket. The timer begins
only after readable request bytes are present.

## What Is Measured

### wolfSSL path

Included:

- socket readability wait completion to first consuming `wolfSSL_read()`
- TLS record parsing and receive path inside wolfSSL
- full request header read
- full request body read and decryption

Excluded:

- TCP accept time
- TLS handshake time
- response generation and response write

### Caddy path

Included:

- socket readability wait completion after handshake
- Go `crypto/tls` inbound record read/decrypt path used by Caddy
- Go `net/http` request parsing on top of the TLS stream
- full request body drain inside a benchmark handler

Excluded:

- TCP accept time
- TLS handshake time
- reverse proxying to faasd or any other upstream
- response generation and response write

## Exact Measurement Points

### wolfSSL side

1. Accept TCP connection
2. Complete TLS handshake
3. Wait until request bytes are readable on the raw socket
4. Stamp `top1`
5. Call `wolfSSL_read()` until headers are complete
6. Parse `Content-Length`
7. Continue `wolfSSL_read()` until the entire request body is consumed
8. Stamp `top2`
9. Send response outside the timed region

### Caddy side

The Caddy implementation should use two instrumentation surfaces.

#### 1. Post-TLS listener wrapper

Use a custom Caddy listener wrapper placed after the TLS placeholder wrapper.
At that position, Caddy has already created the TLS listener.

The wrapper should:

1. accept a TLS connection
2. force `Handshake()` immediately so the handshake is outside the timed region
3. keep a reference to the underlying raw TCP socket
4. wrap the TLS connection with a custom `net.Conn`
5. on the first post-handshake `Read()` that belongs to the HTTP request:
   - wait until the raw socket is readable
   - stamp `top1`
   - let the normal TLS read proceed

This gives the Caddy path the same start rule as wolfSSL: bytes are already in
the kernel receive buffer, and the timed interval starts immediately before the
first real consuming read of the request.

#### 2. Benchmark HTTP handler

Use a dedicated Caddy HTTP handler for the benchmark route.

The handler should:

1. receive the parsed request
2. drain `r.Body` completely
3. confirm the number of bytes matches the expected payload size
4. stamp `top2` immediately after the last body byte is consumed
5. write the benchmark response outside the timed region

This isolates the Caddy inbound request consumption path instead of measuring
reverse proxy overhead.

## Why We Do Not Measure `reverse_proxy`

For this benchmark, the target is request read/decrypt cost, not the cost of
forwarding to an upstream.

If the timed region included `reverse_proxy`, then the measurement would also
include:

- request cloning
- upstream transport logic
- buffering policy
- backend pacing
- upstream response timing

That would contaminate the result and stop being a clean wolfSSL vs Caddy read
comparison.

## Caddy Source Surfaces To Patch

The current implementation plan is based on the following Caddy surfaces:

- `modules/caddyhttp/app.go`
  Caddy starts the HTTP servers here and applies listener wrappers before and
  after TLS listener creation.

- `modules/caddyhttp/caddyhttp.go`
  The TLS placeholder wrapper marks the split between pre-TLS and post-TLS
  listener wrappers.

- `modules/caddyhttp/server.go`
  Caddy exposes `RegisterConnContext()` and `RegisterConnState()` on the server,
  which can be used by a benchmark module to carry per-connection timing state
  into request handling.

- `modules/caddyhttp/server.go`
  Request handling begins in `ServeHTTP`, then enters the primary handler chain.
  The benchmark handler will live in that handler chain and stamp `top2` after
  draining `r.Body`.

## Local Machine Assumptions

- Ubuntu 24.04 or similar Linux host
- Client and server both run on this machine
- Caddy is installed from the official package
- Go 1.25.0 or newer is installed so an instrumented Caddy binary can be built

## Toolchain Note

Current Caddy source requires Go 1.25.0 or newer.

On this machine, the default Ubuntu `golang-go` package is not sufficient for a
source-built benchmark binary, so the local setup should install a newer Go
toolchain from the official Go distribution or another source that provides Go
1.25+.

## Planned Directory Layout

- `client/`
  client sweep runner that sends 100 fresh TLS connections per payload size
- `wolfssl_server/`
  C benchmark server using `wolfSSL_read()` timing
- `scripts/`
  helper scripts for setup, running, and collecting CSV files
- `results/`
  raw and summary CSV outputs

## Planned CSV Fields

- `implementation`
- `payload_size`
- `sample_index`
- `top1_rdtsc`
- `top2_rdtsc`
- `delta_cycles`
- `cntfrq`
- `delta_ns`
- `bytes_expected`
- `bytes_consumed`
- `tls_version`
- `cipher_suite`

## Current Status

This directory currently documents the agreed benchmark contract and the Caddy
instrumentation plan. The next implementation steps are:

1. add the wolfSSL benchmark server
2. add the client sweep runner
3. prepare an instrumented Caddy build path
4. add run scripts and CSV writers