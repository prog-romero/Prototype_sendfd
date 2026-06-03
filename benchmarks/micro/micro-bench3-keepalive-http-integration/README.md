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

### Gateway Build (custom gateway image)

> [!IMPORTANT]
> The gateway build command must run with the `faas/gateway` subdirectory as the build context, rather than the repository root.

```bash
# Run from the repository root on your developer machine:
docker build --no-cache \
  -t romerosdd/openfaas-gateway-ka:latest \
  benchmarks/micro/micro-bench3-keepalive-http-integration/faas/gateway
```

### faasd Build

```bash
cd benchmarks/micro/micro-bench3-keepalive-http-integration/faasd
go build -o faasd ./cmd/...
```

### of-watchdog Build

```bash
cd benchmarks/micro/micro-bench3-keepalive-http-integration/of-watchdog
go build -o fwatchdog ./cmd/...
```



### C Workers Build (from repo root)

```bash
# Build timing-fn-a image (no-cache recommended to force C recompilation)
docker build --no-cache \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-a/Dockerfile \
  -t romerosdd/timing-fn-a-ka-integration:latest .

# Build timing-fn-b image
docker build --no-cache \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-b/Dockerfile \
  -t romerosdd/timing-fn-b-ka-integration:latest .
```

## Push images (to Docker Hub)

```bash
docker push romerosdd/timing-fn-a-ka-integration:latest
docker push romerosdd/timing-fn-b-ka-integration:latest
```

## Deploy

```bash
# Set environment variables
export GATEWAY=http://127.0.0.1:8080
export REGISTRY=docker.io/romerosdd   # optional

bash scripts/deploy.sh
```

Or manually with faas-cli:

```bash
faas-cli deploy \
  --gateway http://127.0.0.1:8080 \
  --image romerosdd/timing-fn-a-ka-integration:latest \
  --name timing-fn-a \
  --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a \
  --env SENDFD_SOCKET_DIR=/run/tlsmigrate \
  --env sendfd_enable=1 \
  --env sendfd_socket_dir=/run/tlsmigrate \
  --fprocess /usr/local/bin/timing-fn-ka-worker
```

## Update C Workers (Rebuild, Push & Deploy)

Whenever you edit the C code for the worker processes, execute the following steps to update the containers running on the Raspberry Pi:

### 1) Build and push the new images on your developer machine:
```bash
# From the repo root:
docker build --no-cache \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-a/Dockerfile \
  -t romerosdd/timing-fn-a-ka-integration:latest .

docker build --no-cache \
  -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-b/Dockerfile \
  -t romerosdd/timing-fn-b-ka-integration:latest .

docker push romerosdd/timing-fn-a-ka-integration:latest
docker push romerosdd/timing-fn-b-ka-integration:latest
```

### 2) Force pull and redeploy the images on your Raspberry Pi:
```bash
# Remove old cached images from containerd's namespace:
sudo ctr -n openfaas-fn images rm docker.io/romerosdd/timing-fn-a-ka-integration:latest || true
sudo ctr -n openfaas-fn images rm docker.io/romerosdd/timing-fn-b-ka-integration:latest || true

# Pull the fresh images from Docker Hub:
sudo ctr -n openfaas-fn image pull docker.io/romerosdd/timing-fn-a-ka-integration:latest
sudo ctr -n openfaas-fn image pull docker.io/romerosdd/timing-fn-b-ka-integration:latest

# Redeploy the functions:
faas-cli deploy -f benchmarks/micro/micro-bench3-keepalive-http-integration/deploy/timing-fn-a.yml
faas-cli deploy -f benchmarks/micro/micro-bench3-keepalive-http-integration/deploy/timing-fn-b.yml
```

## Update the gateway image in faasd

Whenever you update the gateway Go code and want to deploy the changes to `faasd` on the Raspberry Pi:

### 1) Rebuild and push the new gateway image on your developer machine
```bash
# Build the image using the gateway directory as the context (forces Go recompilation)
docker build --no-cache \
  -t romerosdd/openfaas-gateway-ka:latest \
  benchmarks/micro/micro-bench3-keepalive-http-integration/faas/gateway

# Push the new image to Docker Hub
docker push romerosdd/openfaas-gateway-ka:latest
```

### 2) Pull and redeploy on the Raspberry Pi
On the Raspberry Pi, stop `faasd`, clean up references to the old container/image, pull the updated image, and restart:
```bash
# Stop faasd
sudo systemctl stop faasd

# Remove the old gateway container from containerd
sudo ctr -n openfaas container rm gateway 2>/dev/null || true

# Remove the old cached gateway image from both namespaces
sudo ctr -n openfaas images rm docker.io/romerosdd/openfaas-gateway-ka:latest 2>/dev/null || true
sudo ctr -n openfaas-fn images rm docker.io/romerosdd/openfaas-gateway-ka:latest 2>/dev/null || true

# Pull the fresh image from Docker Hub
sudo ctr -n openfaas image pull docker.io/romerosdd/openfaas-gateway-ka:latest

# Start faasd again
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

### 1) Smoke test connection migration manually
You can verify connection migration using the following inline Python command:
```python
python3 -c '
import http.client, json
conn = http.client.HTTPConnection("127.0.0.1", 8080, timeout=10)

print("--> Sending request 1 to /function/timing-fn-a")
conn.request("POST", "/function/timing-fn-a", body="Payload-A", headers={"Connection": "keep-alive"})
r1 = conn.getresponse()
print("Response 1:", r1.status, json.loads(r1.read().decode()))

print("\n--> Sending request 2 to /function/timing-fn-b (on SAME TCP connection)")
conn.request("POST", "/function/timing-fn-b", body="Payload-B", headers={"Connection": "close"})
r2 = conn.getresponse()
print("Response 2:", r2.status, json.loads(r2.read().decode()))
conn.close()
'
```

### 2) Rerunning the Prototype Evaluation Sweep
To run the automated sweep for the prototype mode (with zero-copy migration) for different payload sizes:
```bash
# Run from the repository root on your developer machine:
python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/run_proto_evaluation.py \
  --host 192.168.2.2
```

### 3) Rerunning the Vanilla Evaluation Sweep
To run the evaluation sweep for standard OpenFaaS vanilla mode:
```bash
# Run the standard sweep (predefined payload ranges):
python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/run_vanilla_evaluation.py \
  --host 192.168.2.2

# OR run the custom sweep matching your specific range (e.g. 1 to 300 KiB, step 10, 50 requests):
python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/run_vanilla_evaluation.py \
  --host 192.168.2.2 \
  --start-kb 1 \
  --end-kb 300 \
  --step-kb 10 \
  --requests 50
```

All sweep results are written to:
`benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/results/`

### 3) Run the custom throughput/RPS sweep
To sweep throughput rates using `wrk2` for the prototype mode:
```bash
python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/sweep_throughput_wrk2.py \
  --mode proto \
  --concurrency 100 \
  --payload-kb 1 \
  --duration-s 20 \
  --timeout-s 20 \
  --rates 50,100,150,200,250,300,350,400,450,500,550,600,650,700 \
  --out benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/proto_rate_sweep_2core_32kb.csv
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
