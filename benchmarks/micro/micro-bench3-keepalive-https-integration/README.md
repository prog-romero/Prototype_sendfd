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

The gateway image is cross-compiled for the Pi (arm64) and **must be built from the
repository root**: the Dockerfile `COPY`s `wolfssl/` and the gateway sources by their
repo-root-relative paths, so the gateway directory cannot be used as the build context.
The tag must match the image deployed on the Pi (`romerosdd/gateway-https:latest`):

```bash
# run from the repository root
docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/Dockerfile \
  -t romerosdd/gateway-https:latest \
  --push \
  .


sudo ctr -n openfaas    image pull docker.io/romerosdd/gateway-https:latest

```


```bash
# (c) faasd (provider) -> sendfd + MIGRATE-VERIFY
cd benchmarks/micro/micro-bench3-keepalive-https-integration/faasd && make dist
scp bin/faasd-arm64 romero@192.168.2.2:/tmp/faasd
sudo install -m 755 /tmp/faasd /usr/local/bin/faasd
sudo systemctl restart faasd faasd-provider


``



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

---

## Scale-from-zero for the prototype — build, redeploy & test

The prototype's fast path (peek + `sendfd`) hands `/function/<name>` requests to
the container **before** the gateway router, so the OpenFaaS scale-from-zero
middleware never ran for migrated requests → a stopped container was never
restarted. The fix invokes the **same** `FunctionScaler` in the migrate loops
just before handing off the fd (HTTP **and** HTTPS), and makes the provider
**re-resolve the container IP** if the watchdog socket is dead (a restart gives a
new IP). Two components change → **two rebuilds**:

| Component | Files changed | Artifact to rebuild |
|---|---|---|
| Gateway | `faas/gateway/httpmigrate/{scale,loop,loop_https}.go`, `faas/gateway/main.go` | image `romerosdd/gateway-https:latest` |
| faasd provider | `faasd/pkg/provider/handlers/migrate_dispatch.go` | the `faasd` binary (arm64) |

### 1. Build & push the gateway image (dev machine, from the repo ROOT)

```bash
cd <repo-root>
docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/Dockerfile \
  -t romerosdd/gateway-https:latest --push .
```

### 2. Build the faasd binary for the Pi (arm64) and copy it over

```bash
cd <repo-root>/benchmarks/micro/micro-bench3-keepalive-https-integration/faasd
make dist                    # produces bin/faasd (amd64) AND bin/faasd-arm64
# (fallback if make dist fails: CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -mod=vendor -o bin/faasd-arm64)
ls -la bin/faasd-arm64
scp bin/faasd-arm64 romero@192.168.2.2:/tmp/faasd-new
```

### 3. Redeploy on the Pi

```bash
# a) install the new faasd binary (contains the provider migrate_dispatch fix)
sudo systemctl stop faasd-provider faasd
sudo install -m 0755 /tmp/faasd-new /usr/local/bin/faasd

# b) force a fresh pull of the new gateway image (faasd caches :latest)
sudo ctr -n openfaas image rm docker.io/romerosdd/gateway-https:latest 2>/dev/null || true

# c) REQUIRED: the gateway must have scale_from_zero=true (else the fix is inactive)
grep -n "scale_from_zero" /var/lib/faasd/docker-compose.yaml \
  || echo ">>> ADD  '- scale_from_zero=true'  to the gateway service env <<<"
# and be in prototype mode
sudo grep -i "HTTPMIGRATE_ENABLE\|HTTPS_ENABLE" /var/lib/faasd/docker-compose.yaml

# d) start faasd (recreates the gateway from the fresh image) then the provider
sudo systemctl start faasd
sleep 20
sudo systemctl start faasd-provider
faas-cli list
```

### 4. Test: kill a running container and check it restarts on a request

```bash
FN=graph-pagerank        # any deployed prototype (full-proxy) function

# 1) confirm it is running
sudo ctr -n openfaas-fn task ls | grep "^$FN "

# 2) KILL the task — it goes STOPPED and, on its own, is NEVER restarted
sudo ctr -n openfaas-fn task kill -s SIGKILL "$FN"
sleep 2
sudo ctr -n openfaas-fn task ls | grep "^$FN "        # expect: STOPPED

# 3) send ONE request via the migrated HTTPS path (8443)
curl -sk -m 30 -w "\nHTTP=%{http_code}\n" -X POST -H "Content-Type: application/json" \
  --data '{"size":510,"seed":42}' https://192.168.2.2:8443/function/"$FN"

# 4) it should now be RUNNING again with a NEW PID -> the fix worked
sudo ctr -n openfaas-fn task ls | grep "^$FN "        # expect: RUNNING

# 5) confirm the scaler fired in the gateway logs
sudo journalctl -t openfaas:gateway -n 300 --no-pager | grep -iE "Scale.*$FN|Ready.*$FN"
```

Expected logs: `[Scale 0/20] function=graph-pagerank 0 => 1 requested` then
`[Ready] function=graph-pagerank waited for - <t>s`. **Before** the fix, step 3
returned `HTTP=000` and the task stayed `STOPPED`. The same test works on the
plain-HTTP path (port `8080`).

> **Known limitation (by design):** this restores scale-from-zero only for the
> **first request of each connection** (the one that triggers migration). Once the
> fd is migrated, keep-alive requests flow directly between client and container
> and bypass the gateway, so a container that dies **mid-session** is not
> auto-recovered until the client opens a **new** connection — inherent to the
> `sendfd` data path.

