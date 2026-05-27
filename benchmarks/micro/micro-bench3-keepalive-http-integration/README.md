# micro-bench3-keepalive-http-integration

Integration variant of **micro-bench3-keepalive-http** that runs on the real
faasd / OpenFaaS gateway stack instead of the standalone prototype binaries.

## What this measures

End-to-end latency of keepalive HTTP request dispatch via `SCM_RIGHTS` FD
transfer between the gateway, the of-watchdog sidecar, and the C function
worker, running inside real containerd containers managed by faasd.

## Architecture

```
Client (Python)
    │  HTTP POST /function/timing-fn-a  (keep-alive)
    ▼
OpenFaaS Gateway (:8080)  [httpmigrate.RunLoop]
    │  top1 = time.Now().UnixNano()   ← stamped before MSG_PEEK
    │  MSG_PEEK → parseFunctionName → "timing-fn-a"
    │  ResolveContainerIP("timing-fn-a") → 10.62.0.5
    │  EnsureRelaySocket(10.62.0.5, ...)
    │  os.Pipe() → pipeRead, pipeWrite
    │  sendmsg(/run/tlsmigrate/10.62.0.5.sock, [clientFD, pipeWrite], payload)
    ▼
of-watchdog (container 10.62.0.5)  [sendfd_server.go]
    │  recvmsg → [clientFD, pipeWrite] + payload
    │  sendmsg(/run/tlsmigrate/10.62.0.5-fn.sock, [clientFD, pipeWrite], payload)
    ▼
C worker (timing-fn-ka-worker)
    │  recvmsg → clientFD + pipeWrite + payload
    │  top1 from payload.top1_rdtsc (ns)
    │  read HTTP body, compute delta_ns = top2 - top1
    │  write JSON response to clientFD
    │  write(pipeWrite, &top2, 8); close(pipeWrite)
    ▼
Gateway relay goroutine
    │  readCompletionPipe → elapsed = time.Since(start)
    │  notifier("timing-fn-a", elapsed)  → Prometheus
```

On subsequent keepalive requests (wrong owner, i.e. `timing-fn-b` comes
after `timing-fn-a`):

```
C worker detects wrong owner, relays clientFD (1 FD) to:
    /run/tlsmigrate/10.62.0.5-relay.sock
    ▼
Gateway relay loop → resolves new IP → EnsureRelaySocket → os.Pipe → sendmsg
    ▼
new container's watchdog → C worker for timing-fn-b
```

## Timing convention

Both gateway and C workers use **nanoseconds**:

| Side    | Tool                         | `cntfrq`        |
|---------|------------------------------|-----------------|
| Gateway | `time.Now().UnixNano()`      | 1 000 000 000   |
| Worker  | `clock_gettime(CLOCK_MONOTONIC_RAW)` | 1 000 000 000 |

`delta_ns = top2 - top1` directly (no scaling needed).

This is compatible with the CSV format of `micro-bench3-keepalive-http`.

## Directory layout

```
micro-bench3-keepalive-http-integration/
├── client/
│   └── run_keepalive_sweep.py      # Python bench client (gateway port 8080)
├── faas/gateway/
│   ├── httpmigrate/                # New Go package (6 files)
│   │   ├── payload.go              # KAPayload struct + marshal/unmarshal
│   │   ├── sendfd.go               # sendfd2WithState / recvfd1WithState (Linux)
│   │   ├── relay_server.go         # EnsureRelaySocket + relay loop (Linux)
│   │   ├── chan_listener.go        # net.Listener backed by channel
│   │   ├── conn_rw.go              # net.Conn wrapper replaying peeked bytes
│   │   └── loop.go                 # RunLoop (Linux)
│   ├── main.go                     # Modified: HTTPMIGRATE_ENABLE=1 path
│   └── Dockerfile                  # COPY httpmigrate added
├── faasd/
│   ├── cmd/provider.go             # /system/function-ip/ route registered
│   └── pkg/provider/handlers/
│       ├── ip_endpoint.go          # MakeFunctionIPHandler
│       └── deploy.go               # /var/lib/faasd/tlsmigrate bind-mount
├── of-watchdog/
│   ├── config/config.go            # SendFDEnable + SendFDSocketDir fields
│   └── pkg/
│       ├── sendfd_server.go        # StartSendFDServer (Linux, SOCK_SEQPACKET)
│       └── watchdog.go             # go StartSendFDServer(...) hook
├── proto_function/
│   ├── timing-fn-a/
│   │   ├── timing_fn_ka_worker.c   # C worker (IP-based socket, 2-FD recv)
│   │   └── Dockerfile
│   └── timing-fn-b/
│       ├── timing_fn_ka_worker.c
│       └── Dockerfile
└── scripts/
    └── deploy.sh                   # faas-cli deploy with env vars
```

## Build

### Gateway

```bash
cd benchmarks/micro/micro-bench3-keepalive-http-integration/faas/gateway
docker build -t openfaas-gateway-ka-integration:latest .
```

### faasd

```bash
cd benchmarks/micro/micro-bench3-keepalive-http-integration/faasd
go build -o faasd ./cmd/...
```

### of-watchdog

```bash
cd benchmarks/micro/micro-bench3-keepalive-http-integration/of-watchdog
go build -o fwatchdog ./cmd/...
```

### Gateway build (custom gateway image)

```bash
docker build \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/faas/gateway/Dockerfile \
  -t timing-gateway-ka-integration:latest .
```

### C workers (from repo root)

```bash
# timing-fn-a
docker build \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-a/Dockerfile \
  -t timing-fn-a-ka-integration:latest .

# timing-fn-b
docker build \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-b/Dockerfile \
  -t timing-fn-b-ka-integration:latest .
```

## Deploy

```bash
# Set environment variables
export GATEWAY=http://127.0.0.1:8080
export REGISTRY=myregistry.io/bench   # optional

bash scripts/deploy.sh
```

Or manually with faas-cli:

```bash
faas-cli deploy \
  --gateway http://127.0.0.1:8080 \
  --image timing-fn-a-ka-integration:latest \
  --name timing-fn-a \
  --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a \
  --env SENDFD_SOCKET_DIR=/run/tlsmigrate \
  --env sendfd_enable=1 \
  --env sendfd_socket_dir=/run/tlsmigrate \
  --fprocess /usr/local/bin/timing-fn-ka-worker
```

## Update the gateway image in faasd

When you rebuild the gateway image and want `faasd` to use the new image, do the following exactly.

### 1) Stop faasd

```bash
sudo systemctl stop faasd
```

### 2) Remove the old gateway container from faasd

```bash
sudo ctr -n openfaas container rm gateway 2>/dev/null || true
```

### 3) Remove the old gateway image from containerd

```bash
sudo ctr -n openfaas images rm docker.io/romerosdd/openfaas-gateway-ka:latest 2>/dev/null || true
```

- This command may leave the image if a container still references it.
- You must remove the container first, then remove the image.

### 4) Pull the fresh gateway image into faasd's containerd namespace

```bash
sudo ctr -n openfaas image pull docker.io/romerosdd/openfaas-gateway-ka:latest
```

### 5) Start faasd again

```bash
sudo systemctl start faasd
```

### 6) Verify the gateway container and image

```bash
sudo ctr -n openfaas container ls
sudo ctr -n openfaas images ls | grep romerosdd/openfaas-gateway-ka
```

### 7) Confirm the patched gateway is active

```bash
curl -s http://127.0.0.1:8080/function/timing-fn-a -d 'test' | python3 -m json.tool
```

- A correctly patched gateway returns monotonic nanosecond `top1_rdtsc` values.
- If `top1_rdtsc` is still 19 digits, the old gateway behavior is still running.

## Run the bench

```bash
python3 client/run_keepalive_sweep.py \
  --host 127.0.0.1 \
  --port 8080 \
  --mode switch \
  --start-kb 32 --end-kb 512 --step-kb 32 \
  --requests 50 \
  --out results_integration.csv
```

## Shared socket directory

The gateway and function containers communicate via UNIX sockets in
`/var/lib/faasd/tlsmigrate` on the host (bind-mounted as `/run/tlsmigrate`
inside each container).

faasd automatically creates this directory and the bind-mount when
`HTTPMIGRATE_ENABLE=1` is set in the faasd process environment.

## Environment variables

| Component   | Variable                     | Default               | Effect                     |
|-------------|------------------------------|-----------------------|----------------------------|
| faasd       | `HTTPMIGRATE_ENABLE`         | —                     | Enables IP-lookup endpoint and bind-mount |
| gateway     | `HTTPMIGRATE_ENABLE`         | —                     | Switches to `RunLoop` path |
| of-watchdog | `sendfd_enable`              | `false`               | Starts the sendfd server   |
| of-watchdog | `sendfd_socket_dir`          | `/run/tlsmigrate`     | Socket directory           |
| C worker    | `HTTPMIGRATE_KA_FUNCTION_NAME` | `timing-fn-a`       | This function's name       |
| C worker    | `SENDFD_SOCKET_DIR`          | `/run/tlsmigrate`     | Socket directory           |
