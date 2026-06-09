# micro-bench3-keepalive-https-integration

Integration variant of **micro-bench3-keepalive-https** that runs on the real
faasd / OpenFaaS gateway stack with HTTPS enabled. It uses **wolfSSL session export/import**
to support zero-handshake TLS session migration across keepalive requests.

## What this measures

End-to-end latency of HTTPS keepalive request dispatch via `SCM_RIGHTS` FD transfer 
and TLS session state migration between the gateway, the of-watchdog sidecar, and the C function 
worker, running inside real containerd containers managed by faasd.

---

## Architecture

```
Client (Python HTTPS)
    │  HTTPS POST /function/timing-fn-a  (keep-alive, TLSv1.3)
    ▼
OpenFaaS Gateway (:8443)  [tls_listener.c / RunLoop]
    │  1. Receives TLS connection, performs handshake.
    │  2. Decrypts first request.
    │  3. Exports TLS session state with wolfSSL_tls_export() -> serial blob
    │  4. Stretches / packages state into tlspeek_serial_t
    │  5. sendmsg(/run/tlsmigrate/10.62.0.5.sock, [clientFD, pipeWrite], serial)
    ▼
of-watchdog (container 10.62.0.5) [sendfd_server.go]
    │  recvmsg → [clientFD, pipeWrite] + serial
    │  sendmsg(/run/tlsmigrate/10.62.0.5-fn.sock, [clientFD, pipeWrite], serial)
    ▼
C worker (timing-fn-ka-worker)
    │  1. recvmsg → clientFD + pipeWrite + serial
    │  2. Restores session state: tlspeek_restore(s->ssl, clientFD, serial)
    │  3. Read HTTP body, compute elapsed delta.
    │  4. Write encrypted HTTPS response directly to clientFD
    │  5. Clear fd from epoll, send completion to pipeWrite, and close.
```

On subsequent keepalive requests (where the next request goes to `timing-fn-b` instead of `timing-fn-a`):

```
C worker (timing-fn-a)
    │  1. Performs stateless peek via MSG_PEEK to read next request header.
    │  2. Detects request is for "/function/timing-fn-b".
    │  3. Exports TLS session state -> new serial blob.
    │  4. Relays clientFD (1 FD) + serial blob to /run/tlsmigrate/10.62.0.5-relay.sock
    ▼
Gateway relay loop
    │  1. Receives relayed clientFD + serial blob.
    │  2. Resolves new container IP for "timing-fn-b".
    │  3. Passes clientFD + new serial to timing-fn-b.
    ▼
C worker (timing-fn-b)
    │  1. Restores the TLS session from the relayed serial state.
    │  2. Processes request 2.
```

---

## Directory Layout

```
micro-bench3-keepalive-https-integration/
├── deploy/
│   ├── faas-stack.yml              # Stack file containing all timing/vanilla functions
│   ├── timing-fn-a.yml             # Single deployment files
│   ├── timing-fn-b.yml
│   ├── sumprod-timing-fn-a.yml     # SumProd variants (matrix multiplications)
│   ├── sumprod-timing-fn-b.yml
│   ├── sumprod-vanilla-fn-a.yml
│   └── sumprod-vanilla-fn-b.yml
├── evaluation/
│   ├── results/                    # Location where output CSVs are stored
│   ├── plots/                      # Generated comparison plots
│   ├── run_keepalive_sweep.py      # Core HTTPS keep-alive client
│   ├── run_proto_evaluation.py     # Script to automate prototype sweep
│   ├── run_vanilla_evaluation.py   # Script to automate vanilla sweep
│   └── plot_vanilla_vs_proto.py    # Python matplotlib plotting utility
├── faas/gateway/
│   ├── httpmigrate/                # Cgo wolfSSL bridge and TCP migration listener
│   │   ├── tls_listener.c          # Gateway TLS handshake and state export logic
│   │   ├── tls_listener.go
│   │   └── payload_https.go        # HTTPS serialization structures
│   ├── main.go                     # Unified gateway entrypoint
│   └── Dockerfile                  # Multi-stage build for the HTTPS gateway
├── proto_function/
│   ├── timing-fn-a/
│   │   ├── timing_fn_ka_worker_https.c  # Non-blocking epoll C worker (https)
│   │   └── Dockerfile
│   └── timing-fn-b/
│       ├── timing_fn_ka_worker_https.c
│       └── Dockerfile
```

---

## Build Instructions

### 1. Gateway Build (on developer machine)

The gateway build uses the gateway directory context to build the custom HTTPS gateway:

```bash
docker buildx build --platform linux/arm64 \
  -t romerosdd/gateway-https:latest \
  benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway \
  --push
```

### 2. C Workers Build (from repo root)

We build both the standard `timing-fn` and the throughput-focused `sumprod-timing-fn` workers using `docker buildx`:

```bash
# Build & push Standard timing functions
docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/timing-fn-a/Dockerfile \
  -t romerosdd/timing-fn-a-ka-https:latest --push .

docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/timing-fn-b/Dockerfile \
  -t romerosdd/timing-fn-b-ka-https:latest --push .

# Build & push SumProd timing functions
docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/sumprod-timing-fn-a/Dockerfile \
  -t romerosdd/sumprod-timing-fn-a-ka-https:latest --push .

docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/sumprod-timing-fn-b/Dockerfile \
  -t romerosdd/sumprod-timing-fn-b-ka-https:latest --push .
```

---

## Deploying to the Pi

### 1. Update/Clean Images on the Raspberry Pi
On the Raspberry Pi, remove the old cached images from containerd to ensure the newest ones are pulled:

```bash
# Remove old references
sudo ctr -n openfaas-fn images rm docker.io/romerosdd/timing-fn-a-ka-https:latest || true
sudo ctr -n openfaas-fn images rm docker.io/romerosdd/timing-fn-b-ka-https:latest || true

# Force pull updated images
sudo ctr -n openfaas-fn image pull docker.io/romerosdd/timing-fn-a-ka-https:latest
sudo ctr -n openfaas-fn image pull docker.io/romerosdd/timing-fn-b-ka-https:latest
```

### 2. Deploy Functions
Deploy the target functions using `faas-cli`:

```bash
# Standard timing functions (for latency evaluation)
faas-cli up -f deploy/timing-fn-a.yml
faas-cli up -f deploy/timing-fn-b.yml

# SumProd functions (for throughput evaluation)
faas-cli up -f deploy/sumprod-timing-fn-a.yml
faas-cli up -f deploy/sumprod-timing-fn-b.yml
```

---

## Running the Evaluation

Ensure the Gateway is running in the correct mode in `/var/lib/faasd/docker-compose.yaml` (restart via `sudo systemctl restart faasd` if changed).

### 1. Smoke Test Connection Migration (HTTPS)
You can quickly check if migration works over TLS without crashes:

```python
python3 -c '
import http.client, json, ssl
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

conn = http.client.HTTPSConnection("127.0.0.1", 8443, timeout=10, context=ctx)
print("--> Sending request 1 to /function/timing-fn-a")
conn.request("POST", "/function/timing-fn-a", body="32 512", headers={"Connection": "keep-alive"})
r1 = conn.getresponse()
print("Response 1:", r1.status, r1.read().decode())

print("\n--> Sending request 2 to /function/timing-fn-b on SAME TLS socket")
conn.request("POST", "/function/timing-fn-b", body="32 512", headers={"Connection": "close"})
r2 = conn.getresponse()
print("Response 2:", r2.status, r2.read().decode())
conn.close()
'
```

### 2. Run Latency Benchmark Sweeps
Run these commands from your local machine to collect CSV latency results.

**Prototype Mode Sweep:**
*Ensure `HTTPMIGRATE_ENABLE=1` & `VANILLA_TIMING_ENABLE=0` in the gateway env.*
```bash
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/run_proto_evaluation.py \
    --host 192.168.2.2 \
    --port 8443 \
    --start-kb 32 \
    --end-kb 512 \
    --step-kb 32 \
    --requests 50 \
    --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/results/proto_results_custom.csv
```

**Vanilla Mode Sweep:**
*Ensure `HTTPMIGRATE_ENABLE=0` & `VANILLA_TIMING_ENABLE=1` in the gateway env, and deploy vanilla functions.*
```bash
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/run_vanilla_evaluation.py \
    --host 192.168.2.2 \
    --port 8443 \
    --start-kb 32 \
    --end-kb 512 \
    --step-kb 32 \
    --requests 50 \
    --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/results/vanilla_results_custom.csv
```

### 3. Generate Comparative Plots
Plot a comparative bar chart from the generated custom sweeps:

```bash
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/plot_vanilla_vs_proto.py \
    --vanilla benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/results/vanilla_results_custom.csv \
    --proto benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/results/proto_results_custom.csv \
    --stat mean \
    --title "HTTPS Keep-Alive Migration: Vanilla vs Prototype" \
    --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation/plots/custom_comparison.png
```
