# micro-bench-max-throughput — Run Instructions

## Overview

This benchmark sweeps wrk concurrency levels (1–128) against the vanilla
wolfSSL TLS gateway and records RPS, latency percentiles, and transfer rate
for each concurrency level.  You repeat for each (num_cores × mode) combo.

**Client machine**: `tchiaze-IdeaPad` (this laptop)  
**Pi server**: `romero@192.168.2.2` (Raspberry Pi 4, ARM64)

---

## Prerequisites

### 1. NAT — required every time the client reboots

faasd on the Pi makes an outbound EULA check on startup.  Without NAT the Pi
has no internet and faasd fails to start.

```bash
# Run on client machine after every reboot:
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward
sudo iptables -t nat -A POSTROUTING -s 192.168.2.0/24 -o wlp2s0 -j MASQUERADE

# Restore Pi's default route if it was lost (run on Pi or via SSH):
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ip route replace default via 192.168.2.10"
```

### 2. Verify faasd is running

```bash
ssh romero@192.168.2.2 'systemctl is-active faasd faasd-provider'
# both should print "active"
```

If faasd is not active:
```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S systemctl restart faasd"
sleep 10
curl -sk https://192.168.2.2:8444/function/bench2-fn-a | head -c 80
```

### 3. Verify function containers are running

```bash
ssh romero@192.168.2.2 'echo tchiaze2003 | sudo -S ctr -n openfaas-fn t ls'
# Expected: bench2-fn-a  <pid>  RUNNING
#           bench2-fn-b  <pid>  RUNNING
```

If missing, redeploy (image valid until ~11:44 on May 4 2026):
```bash
IMAGE=ttl.sh/bench3-keepalive-vanilla-fn-20260503114431:24h
ssh romero@192.168.2.2 "
  faas-cli deploy --image '$IMAGE' --name bench2-fn-a --gateway http://127.0.0.1:8080 \
    --env BENCH2_WORKER_NAME=bench2-fn-a --env BENCH2_LISTEN_PORT=8080 \
    --label com.openfaas.scale.zero=false
  faas-cli deploy --image '$IMAGE' --name bench2-fn-b --gateway http://127.0.0.1:8080 \
    --env BENCH2_WORKER_NAME=bench2-fn-b --env BENCH2_LISTEN_PORT=8080 \
    --label com.openfaas.scale.zero=false
"
```

After the ttl.sh image expires, rebuild with:
```bash
bash benchmarks/micro/micro-bench-max-throughput/scripts/build_push_deploy_vanilla_function.sh
```

### 4. Smoke test

```bash
curl -sk https://192.168.2.2:8444/function/bench2-fn-a
# Should return JSON: {"worker":"bench2-fn-a","request_no":1,...}
```

---

## Running the benchmark

### Step 1 — SCP the pinning script to the Pi (only needed once per session)

```bash
scp benchmarks/micro/micro-bench-max-throughput/scripts/pi_pin_all.sh \
    romero@192.168.2.2:/tmp/
```

### Step 2 — For each core count, pin the server, then run both sweeps

The full sweep runs 12 concurrency levels × 20 s each + 3 s pauses ≈ **5 min per sweep**.
Total for all 4 × 2 = 8 runs ≈ **40 min**.

```bash
SCRIPT_DIR=benchmarks/micro/micro-bench-max-throughput/scripts
RESULTS_DIR=benchmarks/micro/micro-bench-max-throughput/results
mkdir -p "$RESULTS_DIR"

for CORES in 1 2 3 4; do
    echo "=== Pinning to ${CORES} core(s) ==="
    ssh romero@192.168.2.2 \
        "echo tchiaze2003 | sudo -S env NUM_CORES=${CORES} bash /tmp/pi_pin_all.sh"

    echo "--- vanilla sweep (${CORES} cores) ---"
    python3 "$SCRIPT_DIR/sweep_throughput.py" \
        32 ${CORES} single \
        "$RESULTS_DIR/vanilla_${CORES}core_32kb.csv"

    echo "--- proto sweep (${CORES} cores) ---"
    python3 "$SCRIPT_DIR/sweep_throughput.py" \
        32 ${CORES} proto \
        "$RESULTS_DIR/proto_${CORES}core_32kb.csv"
done
```

Or run a single combination manually:
```bash
# Pin to 4 cores
ssh romero@192.168.2.2 \
    "echo tchiaze2003 | sudo -S env NUM_CORES=4 bash /tmp/pi_pin_all.sh"

# Vanilla sweep, 4 cores, 32 KB payload
python3 benchmarks/micro/micro-bench-max-throughput/scripts/sweep_throughput.py \
    32 4 single \
    benchmarks/micro/micro-bench-max-throughput/results/vanilla_4core_32kb.csv

# Proto sweep, 4 cores, 32 KB payload
python3 benchmarks/micro/micro-bench-max-throughput/scripts/sweep_throughput.py \
    32 4 proto \
    benchmarks/micro/micro-bench-max-throughput/results/proto_4core_32kb.csv
```

---

## Output CSV columns

| Column           | Description                                      |
|------------------|--------------------------------------------------|
| `concurrency`    | Number of concurrent wrk connections             |
| `rps`            | Requests per second                              |
| `transfer_mb_s`  | Throughput in MB/s                               |
| `lat_avg_ms`     | Average latency (ms)                             |
| `lat_stdev_ms`   | Latency standard deviation (ms)                 |
| `lat_max_ms`     | Maximum latency (ms)                             |
| `p50_ms`         | 50th percentile latency (ms)                     |
| `p75_ms`         | 75th percentile latency (ms)                     |
| `p90_ms`         | 90th percentile latency (ms)                     |
| `p99_ms`         | 99th percentile latency (ms)                     |
| `total_requests` | Total requests completed in the step             |
| `read_mb`        | Total data read (MB)                             |
| `errors_non2xx`  | Non-2xx HTTP responses                          |
| `payload_kb`     | POST body size (KB)                              |
| `num_cores`      | Server CPU cores pinned                          |
| `mode`           | `single` (vanilla :8444) or `proto` (:9444)     |

---

## Troubleshooting

### Gateway port 8444 disappears after wrk starts

The deployed gateway binary had a bug where a failed TLS handshake would kill
the goroutine.  The fix is compiled into the current image
(`docker.io/local/bench3-keepalive-gateway:arm64`).  If this happens again,
restart faasd and **rebuild the gateway**:

```bash
ENABLE_ON_PI=1 PI_SUDO_PASSWORD=tchiaze2003 REQUIRE_LOCAL_CACHE=0 BUILD_PROGRESS=plain \
  bash benchmarks/micro/micro-bench-max-throughput/scripts/build_deploy_vanilla_gw.sh
```

### faasd fails to start (EULA check)

Make sure NAT is enabled on the client (see Prerequisites §1).

### sweep_throughput.py skips all steps

Check that `curl -sk https://192.168.2.2:8444/function/bench2-fn-a` returns
JSON.  If not, restart faasd and redeploy functions (see Prerequisites §2–3).

### Proto mode (port 9444) not reachable

Deploy the proto gateway:
```bash
ENABLE_ON_PI=1 PI_SUDO_PASSWORD=tchiaze2003 REQUIRE_LOCAL_CACHE=0 BUILD_PROGRESS=plain \
  bash benchmarks/micro/micro-bench-max-throughput/scripts/build_deploy_proto_gw.sh
```

---

## Files

```
benchmarks/micro/micro-bench-max-throughput/
├── client/
│   └── post_payload.lua          # wrk POST body script (32 KB default)
├── scripts/
│   ├── sweep_throughput.py       # main sweep runner → CSV
│   ├── pi_pin_all.sh             # CPU affinity pinning on Pi
│   ├── build_deploy_vanilla_gw.sh  # rebuild + deploy vanilla gateway
│   └── build_deploy_proto_gw.sh    # rebuild + deploy proto gateway
├── results/                      # CSV output directory
└── RUN_INSTRUCTIONS.md           # this file
```
