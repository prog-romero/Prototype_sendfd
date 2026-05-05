# Micro-Benchmark: Max Throughput (Debit Max)

Evaluates max sustained throughput (KB/s and MB/s) and latency under increasing
concurrency, comparing:

- Vanilla mode: direct TLS path on port 8444
- Prototype mode: fd-passing path on port 9444

Client: tchiaze-IdeaPad (192.168.2.10)  
Pi server: romero@192.168.2.2 (Raspberry Pi 4, ARM64)

---

## At-a-glance run commands

Vanilla full sweep:

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
bash benchmarks/micro/micro-bench-max-throughput/scripts/run_vanilla_sweep.sh
```

Prototype full sweep:

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
bash benchmarks/micro/micro-bench-max-throughput/scripts/run_proto_sweep.sh
```

If you only remember one thing: always do the pre-run checklist below first.

---

## Pre-run checklist (do before any evaluation)

### 1. Enable NAT (required)

faasd needs outbound internet (EULA/startup checks, ttl.sh pull/push).

```bash
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward
sudo iptables -t nat -A POSTROUTING -s 192.168.2.0/24 -o wlp2s0 -j MASQUERADE
```

Optional route fix on Pi:

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ip route replace default via 192.168.2.10"
```

### 2. Confirm Pi services are up

```bash
ssh romero@192.168.2.2 "systemctl is-active faasd faasd-provider"
```

Expected: both are `active`.

### 3. Verify current gateway mode (vanilla vs proto)

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S bash -c 'ctr -n openfaas containers info gateway 2>/dev/null | grep -E \"Image|BENCH2GW_ENABLE|BENCH2_TLS_LISTEN\"; echo ---; ss -tlnp | grep -E \":8444|:9444\"'"
```

Interpretation:

- Vanilla: `BENCH2GW_ENABLE=0` and `:8444` listening
- Proto: `BENCH2GW_ENABLE=1` and `:9444` listening

### 4. Check local tools on client

```bash
command -v python3
command -v wrk
command -v ssh
```

---

## Full procedure: Vanilla evaluation

### A. Ensure gateway is in vanilla mode

If mode check says proto, restore vanilla first:

```bash
scp benchmarks/micro/micro-bench-max-throughput/scripts/pi_restore_vanilla_gw.sh romero@192.168.2.2:/tmp/pi_restore_vanilla_gw.sh
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env PI_SUDO_PASSWORD=tchiaze2003 bash /tmp/pi_restore_vanilla_gw.sh"
```

Then verify:

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ss -tlnp | grep -E ':8444|:9444|:8080'"
curl -vsk --max-time 8 https://192.168.2.2:8444/healthz 2>&1 | tail -6
```

### B. Ensure vanilla function image is valid (ttl.sh may expire)

If needed, rebuild and push:

```bash
bash benchmarks/micro/micro-bench-max-throughput/scripts/build_push_deploy_vanilla_function.sh
```

If a new image tag is generated, set it when running sweep:

```bash
FN_IMAGE=<new_ttl_tag> bash benchmarks/micro/micro-bench-max-throughput/scripts/run_vanilla_sweep.sh
```

### C. Run vanilla sweep

```bash
bash benchmarks/micro/micro-bench-max-throughput/scripts/run_vanilla_sweep.sh
```

What this script does per core (1 to 4):

- restart faasd
- wait API
- redeploy bench2-fn-a and bench2-fn-b
- wait health on 8444
- repin containers
- run sweep in alternate mode

Outputs:

- benchmarks/micro/micro-bench-max-throughput/results/vanilla_alt_1core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/vanilla_alt_2core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/vanilla_alt_3core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/vanilla_alt_4core_32kb_v2.csv

---

## Full procedure: Prototype evaluation

### A. Ensure gateway image exists on Pi

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ctr -n openfaas image ls | grep bench3-keepalive-gateway"
```

If missing/broken, rebuild gateway image:

```bash
ENABLE_ON_PI=1 PI_SUDO_PASSWORD=tchiaze2003 REQUIRE_LOCAL_CACHE=0 \
  bash benchmarks/micro/micro-bench-max-throughput/scripts/build_deploy_vanilla_gw.sh
```

### B. Ensure proto worker image exists locally on Pi (source image for push)

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ctr -n openfaas image ls | grep bench2-proto-worker"
```

If missing, build proto worker image first (project-specific build path).

### C. Run proto sweep

```bash
bash benchmarks/micro/micro-bench-max-throughput/scripts/run_proto_sweep.sh
```

What this script does automatically:

- uploads helper scripts to Pi
- pushes `docker.io/local/bench2-proto-worker:arm64` to ttl.sh
- enables proto gateway mode
- per core: restart/redeploy/health/pin/sweep on 9444
- restores vanilla mode at the end

Outputs:

- benchmarks/micro/micro-bench-max-throughput/results/proto_alt_1core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/proto_alt_2core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/proto_alt_3core_32kb_v2.csv
- benchmarks/micro/micro-bench-max-throughput/results/proto_alt_4core_32kb_v2.csv

---

## Manual mode switching (recovery)

Enable proto gateway manually:

```bash
scp benchmarks/micro/micro-bench-max-throughput/scripts/pi_enable_proto_gw.sh romero@192.168.2.2:/tmp/pi_enable_proto_gw.sh
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env GATEWAY_IMAGE=docker.io/local/bench3-keepalive-gateway:arm64 PI_SUDO_PASSWORD=tchiaze2003 bash /tmp/pi_enable_proto_gw.sh"
```

Restore vanilla gateway manually:

```bash
scp benchmarks/micro/micro-bench-max-throughput/scripts/pi_restore_vanilla_gw.sh romero@192.168.2.2:/tmp/pi_restore_vanilla_gw.sh
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env PI_SUDO_PASSWORD=tchiaze2003 bash /tmp/pi_restore_vanilla_gw.sh"
```

---

## Sweep behavior

- Concurrency levels: 1, 11, 21, ..., 401
- Duration per step: 20 s
- Early stop: plateau after 10 consecutive non-improving steps
- Stabilization before sweep: 15 s
- Inter-step pause: 3 s

---

## Output CSV columns

| Column           | Description                                    |
|------------------|------------------------------------------------|
| concurrency      | Number of concurrent wrk connections           |
| rps              | Requests per second                            |
| transfer_kb_s    | Throughput in KB/s                             |
| transfer_mb_s    | Throughput in MB/s                             |
| lat_avg_ms       | Average latency (ms)                           |
| lat_stdev_ms     | Latency stdev (ms)                             |
| lat_max_ms       | Max latency (ms)                               |
| p50_ms           | 50th percentile latency (ms)                   |
| p75_ms           | 75th percentile latency (ms)                   |
| p90_ms           | 90th percentile latency (ms)                   |
| p99_ms           | 99th percentile latency (ms)                   |
| total_requests   | Total completed requests                       |
| read_mb          | Total data read (MB)                           |
| errors_non2xx    | Non-2xx responses                              |
| payload_kb       | POST body size                                 |
| num_cores        | Pinned core count                              |
| mode             | vanilla or proto gateway path                  |
| target_mode      | wrk routing pattern: same or alternate         |

---

## Troubleshooting quick fixes

Function hangs / stale keepalive:

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S systemctl restart faasd"
```

Proto pull errors from ttl.sh:

```bash
ssh romero@192.168.2.2 "curl -fsS https://ttl.sh/ | head -c 20"
```

Gateway mode confusion:

```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S ctr -n openfaas containers info gateway 2>/dev/null | grep BENCH2GW_ENABLE"
```

---

## Files

```text
benchmarks/micro/micro-bench-max-throughput/
├── client/
│   └── post_payload.lua
├── scripts/
│   ├── run_vanilla_sweep.sh
│   ├── run_proto_sweep.sh
│   ├── sweep_throughput.py
│   ├── pi_pin_all.sh
│   ├── pi_enable_proto_gw.sh
│   ├── pi_restore_vanilla_gw.sh
│   ├── build_deploy_vanilla_gw.sh
│   ├── build_deploy_proto_gw.sh
│   └── build_push_deploy_vanilla_function.sh
└── results/
```
