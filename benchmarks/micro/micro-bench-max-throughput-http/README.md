# micro-bench-max-throughput-http — Maximum Throughput (Keep-Alive HTTP)

This benchmark measures the **maximum sustainable throughput** of the
keep-alive HTTP prototype compared with plain faasd (vanilla) under increasing
concurrency. It uses [wrk2](https://github.com/giltene/wrk2) as the load
generator.

Key differences from earlier latency micro-benchmarks:

| Feature | Earlier benchmarks | This benchmark |
|---|---|---|
| Connection mode | one connection per request (`Connection: close`) | persistent connections (`keep-alive`) |
| Goal | measure per-request latency | find saturation throughput |
| Load generator | custom Python client | wrk2 (constant-rate) |
| Worker design | blocking single-session | **epoll multi-session** (non-blocking) |

## 1. Architecture

### Vanilla path

```
wrk2 (keep-alive) --> faasd gateway :8080 --> timing-fn-a / timing-fn-b (faasd container)
```

- faasd forwards each request over a new or pooled HTTP/1.1 connection to the container
- measured at the HTTP response level by wrk2

### Prototype path (keep-alive + `sendfd`)

```
wrk2 (keep-alive) --> proto gateway :8083
                          |
                          | epoll accept + MSG_PEEK (routing)
                          |
                          +--> sendfd (SCM_RIGHTS) --> timing-fn-a worker
                          |                            (epoll multi-session)
                          |
                          +--> sendfd (SCM_RIGHTS) --> timing-fn-b worker
                                                       (epoll multi-session)
```

- the proto gateway accepts the TCP socket, peeks only enough data to parse
  `/function/<name>`, then passes the raw fd to the correct worker via
  `sendfd(SCM_RIGHTS)` over a Unix domain socket
- workers hold the migrated fd in an epoll instance and serve requests directly,
  without ever going back through the gateway or faasd
- wrong-owner fds (keep-alive connection routed to the wrong worker) are relayed
  back to the gateway via a relay Unix socket and re-dispatched

### Worker epoll state machine

Each worker runs a single-threaded epoll event loop monitoring:

- the UDS `listen_fd` for new fd deliveries from the gateway
- all active client fds

Per-client state: `S_READ_HEADERS → S_READ_BODY → S_WRITE_RESP → S_PEEK_OWNER → …`

No blocking calls inside the loop — all `recv`/`write` use `MSG_DONTWAIT`; an
`EAGAIN` simply returns to `epoll_wait`.

## 2. Components

| Path | Description |
|---|---|
| `proto_gateway/benchhttp/httpmigrate_ka.c` | C shim (CGo): epoll accept + peek; relay epoll accept |
| `proto_gateway/benchhttp/server.go` | Go goroutines: dispatch + relay loop |
| `proto_function/timing-fn-a/timing_fn_ka_worker.c` | Worker A — computes `a + b` (SUM), epoll multi-session |
| `proto_function/timing-fn-b/timing_fn_ka_worker.c` | Worker B — computes `a * b` (PRODUCT), epoll multi-session |
| `function/timing-fn-a/`, `function/timing-fn-b/` | Vanilla faasd function containers |
| `scripts/sweep_throughput.py` | Core wrk2 sweep engine (env-var configurable) |
| `scripts/sweep_vanilla_3vars_wrk2.py` | **3-variable evaluation** — sweeps rate / concurrency / payload, writes 3 CSVs, captures Pi CPU per point |
| `scripts/run_vanilla_sweep.sh` | Pinned vanilla sweep (CPUs fixed) |
| `scripts/run_vanilla_sweep_nopin.sh` | Unpinned vanilla sweep |
| `scripts/run_vanilla_single_nopin.sh` | Single-config vanilla run |
| `scripts/run_proto_sweep.sh` | Prototype sweep |
| `scripts/prepare_proto_stack.sh` | Build + push + deploy gateway and both workers |
| `scripts/prepare_vanilla_stack.sh` | Build + push + deploy vanilla functions |

## 3. Vanilla 3-variable evaluation (`sweep_vanilla_3vars_wrk2.py`)

This script performs three independent sweeps on vanilla faasd and collects
Pi CPU usage per test point. Run them in order: rate → concurrency → payload.

For each sweep one variable changes; the other two are held fixed by
`--fixed-*` arguments. Each produces one CSV in `--out-dir`.

### Step 0 — Prerequisites

```bash
# 1. wrk2 binary must exist
ls ~/wrk2/wrk

# 2. vanilla functions must be deployed (NOT the proto sendfd workers)
PI_SSH=romero@192.168.2.2 \
PI_SUDO_PASSWORD=tchiaze2003 \
BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_vanilla_stack.sh

# 3. Verify both endpoints return HTTP 200 before starting any sweep
ssh romero@192.168.2.2 '\
  curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/function/timing-fn-a -X POST -H "Content-Length: 0"; \
  curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/function/timing-fn-b -X POST -H "Content-Length: 0"'
# Expected: 200 (both lines)
```

> The script itself runs a preflight check and exits with a clear message
> if either endpoint is not healthy.

### Step 1 — Rate sweep (vary rate, fix concurrency + payload)

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500,550,600,650,700,750,800 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8080/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_3vars
```

Output: `results/vanilla_3vars/rate_sweep.csv`

### Step 2 — Concurrency sweep (vary concurrency, fix rate + payload)

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8080/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_3vars
```

Output: `results/vanilla_3vars/concurrency_sweep.csv`

### Step 3 — Payload sweep (vary payload size, fix rate + concurrency)

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8080/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_3vars
```

Output: `results/vanilla_3vars/payload_sweep.csv`

### CSV columns (all 3 sweeps)

| Column | Description |
|---|---|
| `sweep_type` | `rate_sweep`, `concurrency_sweep`, or `payload_sweep` |
| `monitored_variable` | Variable being swept in this row |
| `monitored_value` | Value of monitored variable for this row |
| `rate` / `concurrency` / `payload_kb` | Actual values used for this run |
| `rps` | Measured requests/s (wrk2 `Requests/sec`) |
| `transfer_kb_s` / `transfer_mb_s` | Network throughput |
| `lat_avg_ms` / `lat_stdev_ms` / `lat_max_ms` | Latency statistics |
| `p50_ms` … `p99_ms` | Latency percentiles |
| `total_requests` / `read_mb` | Total work done in the step |
| `errors_non2xx` | Non-200 responses |
| `socket_*_errors` | wrk2 socket-level errors |
| `pi_cpu_busy_avg_pct` | Pi CPU busy % averaged over the run (0–400%, 400 = all 4 cores full) |
| `pi_cpu_busy_max_pct` | Peak Pi CPU sample during the run |
| `pi_cpu_busy_min_pct` | Minimum Pi CPU sample during the run |
| `pi_cpu_samples` | Number of `/proc/stat` samples collected |

## 4. Prototype 3-variable evaluation (`sweep_vanilla_3vars_wrk2.py` against `:8083`)

Same script, same three sweeps — but targeting the proto gateway on `:8083`.
The proto gateway must be running (`HTTPMIGRATE_KA_ENABLE=1` in compose) and
both proto worker functions deployed before starting.

### Step 0 — Prerequisites

```bash
# 1. Deploy proto workers + enable proto gateway
PI_SSH=romero@192.168.2.2 \
PI_SUDO_PASSWORD=tchiaze2003 \
BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/build_copy_enable_proto_gateway.sh

# 2. Smoke test — expect 200
ssh romero@192.168.2.2 '\
  curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8083/function/timing-fn-a -X POST -H "Content-Length: 0"; \
  curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8083/function/timing-fn-b -X POST -H "Content-Length: 0"'
# Expected: 200 (both lines)
```

### Step 1 — Rate sweep

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500,550,600,650,700,750,800 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8083/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/proto_3vars
```

Output: `results/proto_3vars/rate_sweep.csv`

### Step 2 — Concurrency sweep

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8083/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/proto_3vars
```

Output: `results/proto_3vars/concurrency_sweep.csv`

### Step 3 — Payload sweep

```bash
python3 benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8083/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/proto_3vars
```

Output: `results/proto_3vars/payload_sweep.csv`

## 5. Environment variables — sweep engine (`sweep_throughput.py`)

| Variable | Default | Description |
|---|---|---|
| `WRK2_RATE` | 100 | Initial target RPS for wrk2 |
| `WRK_DURATION_S` | 30 | Measurement window per step (seconds) |
| `WRK_THREADS` | 4 | wrk2 thread count |
| `WRK2_TIMEOUT_S` | 10 | Per-request timeout (seconds) |
| `NOPIN_STEPS` | `"1:10,1:50,1:100,…"` | `T:C` pairs to sweep |
| `PAUSE_BETWEEN_STEPS_S` | 5 | Cooldown between steps |
| `STABILIZE_BEFORE_SWEEP_S` | 10 | Warm-up before first step |

`NOPIN_STEPS` format: comma-separated `threads:connections` pairs,
e.g. `"1:200"` for a single point, `"2:50,2:100,2:200"` for three points.

## 6. Quick start

### Prerequisites (client machine)

```bash
# wrk2 must be at ~/wrk2/wrk
ls ~/wrk2/wrk

# Python 3 with no extra deps needed
python3 --version
```

### Prerequisites (Pi)

- faasd running on `192.168.2.2`
- OpenFaaS credentials in `~/.config/faas-cli/credentials`
- `faas-cli` available on Pi
- Docker buildx builder `bench2-arm64-builder` on client

### Deploy the prototype stack

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

PI_SSH=romero@192.168.2.2 \
PI_SUDO_PASSWORD=tchiaze2003 \
BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_proto_stack.sh
```

### Smoke-test

```bash
# Vanilla (faasd)
curl -s http://192.168.2.2:8080/function/timing-fn-a

# Prototype gateway
curl -s http://192.168.2.2:8083/function/timing-fn-a
```

### Run a single vanilla point (no CPU pinning)

```bash
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_single_nopin.sh \
  0 2 200
# args: <payload_kb> <threads> <connections>
```

### Run a vanilla throughput sweep (no CPU pinning)

```bash
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_sweep_nopin.sh
```

### Run a prototype throughput sweep

```bash
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_proto_sweep.sh
```

### Custom sweep with env-var overrides

```bash
WRK2_RATE=500 \
WRK_DURATION_S=60 \
NOPIN_STEPS="2:100,2:200,4:400" \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_sweep_nopin.sh
```

## 7. Image build & push (manual)

All images target `linux/arm64` via Docker buildx cross-compilation.

```bash
ROOT=~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
BUILDER=bench2-arm64-builder

# Worker A (epoll, SUM)
docker buildx build --builder $BUILDER --platform linux/arm64 \
  -f benchmarks/micro/micro-bench-max-throughput-http/proto_function/timing-fn-a/Dockerfile \
  -t ttl.sh/timing-fn-a-ka-http:24h --push $ROOT

# Worker B (epoll, PRODUCT)
docker buildx build --builder $BUILDER --platform linux/arm64 \
  -f benchmarks/micro/micro-bench-max-throughput-http/proto_function/timing-fn-b/Dockerfile \
  -t ttl.sh/timing-fn-b-ka-http:24h --push $ROOT

# Gateway (includes httpmigrate_ka.c with relay epoll)
PI_SSH=romero@192.168.2.2 PI_SUDO_PASSWORD=tchiaze2003 \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/build_copy_enable_proto_gateway.sh
```

## 8. Port map

| Port | Service |
|---|---|
| `:8080` | faasd native gateway (vanilla) |
| `:8083` | proto keep-alive gateway (sendfd path) |

## 9. Results

CSV output is written to `results/` by the sweep scripts. Columns:

```
timestamp, mode, payload_kb, threads, connections, target_rps,
actual_rps, p50_ms, p99_ms, p999_ms, errors, timeouts
```

- `errors`: non-2xx responses (typically 503 from faasd queue overflow)
- `timeouts`: wrk2 socket-level timeouts before any HTTP response
- `actual_rps`: measured completed requests per second (wrk2 `Requests/sec`)

## 10. Known limitations

- Worker images use `ttl.sh` (24-hour expiry) — re-push before each session
- The gateway image is loaded directly into containerd on the Pi (not a registry push)
- CPU pinning (`taskset`) must be applied manually via `scripts/pi_pin_all.sh` after each faasd restart
- wrk2 emits `nan` latency when zero requests complete in a step; the sweep engine handles this gracefully (records zeros)
