# micro-bench-max-throughput-http

Maximum-throughput HTTP benchmark with keep-alive connections.

This benchmark supports two modes:
- Vanilla mode: OpenFaaS function path through the benchmark-enabled gateway listener on host port 8082.
- Prototype mode: sendfd keep-alive path through the prototype gateway on host port 8083.

Both modes use wrk2 for load generation and can be evaluated with:
- 3-variable sweep script (rate, concurrency, payload), or
- legacy sweep scripts.

## 1. What this benchmark measures

- Sustained requests/sec under keep-alive HTTP/1.1.
- Latency distribution under increasing offered load.
- Error behavior near saturation.
- Pi CPU busy percentage during each run point (with 3-variable script).

## 2. Ports and endpoints

- 8080: native OpenFaaS gateway on the Pi
- 8082: vanilla benchmark listener (host 8082 -> container 8085)
- 8083: prototype benchmark listener

Benchmark endpoints:
- Vanilla:
  - http://192.168.2.2:8082/function/timing-fn-a
  - http://192.168.2.2:8082/function/timing-fn-b
- Prototype:
  - http://192.168.2.2:8083/function/timing-fn-a
  - http://192.168.2.2:8083/function/timing-fn-b

## 3. Prerequisites

Client machine:

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

# wrk2 binary
ls ~/wrk2/wrk

# Python
python3 --version

# Docker buildx builder
docker buildx inspect bench2-arm64-builder >/dev/null
```

Pi machine assumptions:
- SSH reachable at romero@192.168.2.2
- faasd installed and managed by systemd
- faas-cli available on Pi
- sudo password available for scripts that patch docker-compose

Common environment used by scripts:

```bash
export PI_SSH=romero@192.168.2.2
export PI_SUDO_PASSWORD=tchiaze2003
export BUILDER_NAME=bench2-arm64-builder
```

## 4. Deploy stacks

### 4.1 Deploy Vanilla stack

This builds/pushes vanilla function images, enables vanilla gateway mode, and deploys timing-fn-a and timing-fn-b.

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

PI_SSH=romero@192.168.2.2 \
PI_SUDO_PASSWORD=tchiaze2003 \
BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_vanilla_stack.sh
```

### 4.2 Deploy Prototype stack

This builds/enables the prototype gateway and deploys proto worker images.

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

PI_SSH=romero@192.168.2.2 \
PI_SUDO_PASSWORD=tchiaze2003 \
BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_proto_stack.sh
```

## 5. Smoke tests before running benches

### 5.1 Vanilla smoke test

```bash
curl -s -X POST http://192.168.2.2:8082/function/timing-fn-a -H "Content-Type: text/plain" -d "5 7"
curl -s -X POST http://192.168.2.2:8082/function/timing-fn-b -H "Content-Type: text/plain" -d "5 7"
```

Expected:
- fn-a result is 12
- fn-b result is 35

### 5.2 Prototype smoke test

```bash
curl -s -X POST http://192.168.2.2:8083/function/timing-fn-a -H "Content-Type: text/plain" -d "5 7"
curl -s -X POST http://192.168.2.2:8083/function/timing-fn-b -H "Content-Type: text/plain" -d "5 7"
```

Expected:
- fn-a result is 12
- fn-b result is 35

## 6. Main evaluation: 3-variable sweeps

Script:
- benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py

Despite the script name, it is mode-agnostic: mode is determined by --url.

It produces three CSV files in the output directory:
- rate_sweep.csv
- concurrency_sweep.csv
- payload_sweep.csv

### 6.1 Run all 3 sweeps for Vanilla mode

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

python3 -u benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500 \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8082/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_epoll_3vars
```

### 6.2 Run all 3 sweeps for Prototype mode

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

python3 -u benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500 \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 \
  --fixed-concurrency 100 \
  --fixed-payload-kb 1 \
  --duration-s 30 \
  --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8083/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/proto_epoll_3vars
```

## 7. Legacy sweep scripts

These are still useful when you want the original sweep flows.

### 7.1 Vanilla no-pinning sweep (payload loop)

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_sweep_nopin.sh
```

Optional overrides:

```bash
PAYLOAD_KB_LIST="32 512" \
FN_A_IMAGE="ttl.sh/timing-fn-a-vanilla-ka:24h" \
FN_B_IMAGE="ttl.sh/timing-fn-b-vanilla-ka:24h" \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_sweep_nopin.sh
```

### 7.2 Vanilla pinned sweep

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_vanilla_sweep.sh
```

### 7.3 Prototype core sweep

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_proto_sweep.sh
```

Optional overrides:

```bash
CORES_LIST="1 2" PAYLOAD_KB=32 \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/run_proto_sweep.sh
```

## 8. One-command sequence to run both modes (recommended order)

1. Prepare and run Vanilla 3-variable sweep.
2. Prepare and run Prototype 3-variable sweep.

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd

# Vanilla
PI_SSH=romero@192.168.2.2 PI_SUDO_PASSWORD=tchiaze2003 BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_vanilla_stack.sh

python3 -u benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500 \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 --fixed-concurrency 100 --fixed-payload-kb 1 \
  --duration-s 30 --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8082/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_epoll_3vars

# Prototype
PI_SSH=romero@192.168.2.2 PI_SUDO_PASSWORD=tchiaze2003 BUILDER_NAME=bench2-arm64-builder \
bash benchmarks/micro/micro-bench-max-throughput-http/scripts/prepare_proto_stack.sh

python3 -u benchmarks/micro/micro-bench-max-throughput-http/scripts/sweep_vanilla_3vars_wrk2.py \
  --rate-values 50,100,150,200,250,300,350,400,450,500 \
  --concurrency-values 10,25,50,75,100,150,200,300,400 \
  --payload-values-kb 1,4,8,16,32,64,128,256,512,1024 \
  --fixed-rate 200 --fixed-concurrency 100 --fixed-payload-kb 1 \
  --duration-s 30 --pause-between-s 20 \
  --pi-ssh romero@192.168.2.2 \
  --url http://192.168.2.2:8083/function/timing-fn-a \
  --target-mode alternate \
  --out-dir benchmarks/micro/micro-bench-max-throughput-http/results/proto_epoll_3vars
```

## 9. Results and outputs

Primary output directories (examples):
- benchmarks/micro/micro-bench-max-throughput-http/results/vanilla_epoll_3vars
- benchmarks/micro/micro-bench-max-throughput-http/results/proto_epoll_3vars

3-variable CSV columns include:
- sweep_type, monitored_variable, monitored_value
- rate, concurrency, payload_kb
- rps, transfer_kb_s, transfer_mb_s
- lat_avg_ms, lat_stdev_ms, lat_max_ms
- p50_ms, p75_ms, p90_ms, p95_ms, p99_ms
- total_requests, read_mb
- errors_non2xx
- socket_connect_errors, socket_read_errors, socket_write_errors, socket_timeout_errors
- pi_cpu_busy_avg_pct, pi_cpu_busy_max_pct, pi_cpu_busy_min_pct, pi_cpu_samples

## 10. Troubleshooting quick checks

- Verify listeners:

```bash
ssh romero@192.168.2.2 "ss -tlnp | grep -E '8080|8082|8083|8444|9443'"
```

- If ttl.sh images expired (24h), rerun prepare scripts to rebuild and redeploy.
- If benchmark URL returns non-200, rerun stack prepare then smoke test again.
