# evaluation_throughput — HTTP Keep-Alive Throughput Scaling & Rate Sweep Evaluation

This directory contains the necessary scripts, configurations, and tools to evaluate the performance of the **Vanilla Gateway** versus the **Prototype SCM_RIGHTS Gateway** under different loads.

It supports two types of throughput benchmarks:
1. **Constant-Throughput Rate Sweep (`sweep_throughput_wrk2.py`)** — Sweeps input request rate (`rate`) using `wrk2` while keeping concurrency fixed (identical to your `micro-bench-max-throughput-http` bench).
2. **Concurrency Sweep (`sweep_throughput.py`)** — Sweeps parallel client connections (`concurrency`) using standard `wrk`.

---

## Directory Structure

```
evaluation_throughput/
├── client/
│   └── post_payload.lua        # Lua script for wrk/wrk2 alternating load generation
├── sweep_throughput_wrk2.py    # Python driver running wrk2 rate sweeps + Pi CPU capture
├── compare_two_csv_plots.py    # Plotting tool to compare rate sweeps (produces 5 figures)
├── sweep_throughput.py         # Python driver running wrk concurrency sweeps
├── plot_throughput.py          # Plotting tool to compare concurrency sweeps
├── pi_pin_all.sh               # CPU core-pinning utility (runs on Raspberry Pi)
└── README.md                   # This documentation file
```

---

## Prerequisites

### 1. Install wrk and wrk2
*   **wrk** (for concurrency sweeps):
    ```bash
    sudo apt update
    sudo apt install wrk -y
    ```
*   **wrk2** (for constant-throughput rate sweeps):
    The sweeper looks for `wrk` inside `~/wrk2/` or the global path. Ensure it is compiled and accessible.

### 2. Active Functions on Pi
Ensure both Vanilla and Prototype SCM_RIGHTS functions are deployed on your Raspberry Pi:
*   **Vanilla functions**: `vanilla-fn-a` and `vanilla-fn-b`
*   **Prototype functions**: `timing-fn-a` and `timing-fn-b`

Deploy them with `faas-cli` inside the main deploy directory:
```bash
faas-cli deploy -f deploy/vanilla-fn-a.yml
faas-cli deploy -f deploy/vanilla-fn-b.yml
faas-cli deploy -f deploy/timing-fn-a.yml
faas-cli deploy -f deploy/timing-fn-b.yml
```

---

## Part 1: Running the wrk2 Rate Sweep (Recommended)

This is the sweep where the **X-axis is the rate** and you evaluate throughput, actual RPS, P99/average latency, and Pi CPU usage.

### Step 1: Copy the Pinning Utility to the Pi
```bash
scp benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/pi_pin_all.sh romero@192.168.2.2:/tmp/
```

### Step 2: Pin the Server to Cores
For example, to pin to exactly **2 cores**:
```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env NUM_CORES=2 bash /tmp/pi_pin_all.sh"
```

### Step 3: Run the Sweeps on the Client Machine

1. **Run Vanilla Rate Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/sweep_throughput_wrk2.py \
       --mode vanilla \
       --concurrency 100 \
       --payload-kb 32 \
       --rates 50,100,150,200,250,300,350,400,450,500,550,600,650,700 \
       --out benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/vanilla_rate_sweep_2core_32kb.csv
   ```

2. **Run SCM_RIGHTS Prototype Rate Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/sweep_throughput_wrk2.py \
       --mode proto \
       --concurrency 100 \
       --payload-kb 32 \
       --rates 50,100,150,200,250,300,350,400,450,500,550,600,650,700 \
       --out benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/proto_rate_sweep_2core_32kb.csv
   ```

---

### Step 4: Plot and Compare (Produces 5 Figures)

To generate the exact comparison plots (where **X-axis is rate** and **Y-axis is the chosen metric**), run:

```bash
python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/compare_two_csv_plots.py \
    benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/vanilla_rate_sweep_2core_32kb.csv \
    benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/proto_rate_sweep_2core_32kb.csv \
    --prefix bench_max \
    --out-dir benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/compare_figures_32kb
```

This will automatically create **5 high-resolution figures** inside `compare_figures_32kb/`:
1. **`bench_max_throughput_kb_s.png`** (Y-axis: Throughput in KB/s)
2. **`bench_max_rps.png`** (Y-axis: Actual Requests/sec)
3. **`bench_max_cpu_avg_pct.png`** (Y-axis: Pi CPU average %)
4. **`bench_max_lat_avg_ms.png`** (Y-axis: Average Latency in ms)
5. **`bench_max_total_requests.png`** (Y-axis: Total requests with timeout annotations)

---

## Part 2: Concurrency Sweeps using wrk (Alternative)

If you wish to sweep parallel connections (`concurrency`) instead of rates:

1. **Run Vanilla Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/sweep_throughput.py \
       32 2 vanilla \
       benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/vanilla_concurrency_2core_32kb.csv
   ```
2. **Run SCM_RIGHTS Prototype Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/sweep_throughput.py \
       32 2 proto \
       benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/proto_concurrency_2core_32kb.csv
   ```
3. **Plot Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/plot_throughput.py \
       --vanilla benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/vanilla_concurrency_2core_32kb.csv \
       --proto benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/results/proto_concurrency_2core_32kb.csv \
       --out benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation_throughput/plots/concurrency_comparison_2core.png
   ```
