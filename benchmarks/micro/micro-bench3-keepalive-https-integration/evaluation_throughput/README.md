# evaluation_throughput — HTTPS Keep-Alive Throughput & Per-Component Resource Evaluation

This directory evaluates the **Vanilla Gateway** versus the **Prototype SCM_RIGHTS Gateway**
(unified HTTPS port `8443`) under different loads, and breaks down *where* the CPU/RAM is spent
on the Raspberry Pi (gateway vs faasd vs fwatchdog vs worker, per function).

It supports two types of throughput benchmarks:
1. **Constant-Throughput Rate Sweep (`sweep_throughput_wrk2.py`)** — sweeps the input request
   rate (`rate`) using `wrk2` while keeping concurrency fixed. This is the recommended/primary
   benchmark. It also drives **per-component pidstat sampling** (see below), in lockstep with
   each rate step.
2. **Concurrency Sweep (`sweep_throughput.py`)** — sweeps parallel client connections
   (`concurrency`) using standard `wrk`, finding the max sustainable throughput.

---

## Directory Structure

```
evaluation_throughput/
├── client/
│   └── post_payload.lua          # Lua script for wrk/wrk2 load generation (POST with payload)
├── sweep_throughput_wrk2.py      # Rate sweep driver: wrk2 + Pi CPU capture + per-component pidstat
├── compare_two_csv_plots.py      # Plots results/*.csv (rate sweep comparison, 6 figures)
├── plot_pidstat.py               # Plots pidstat/<mode>/{cpu,ram}/*.csv (per-component figures)
├── sweep_throughput.py           # Concurrency sweep driver using wrk
├── plot_throughput.py            # Plots concurrency sweep comparison
├── pi_pin_all.sh                 # CPU core-pinning utility (runs on the Raspberry Pi)
├── pidstat/
│   ├── collect_pidstat.sh        # Standalone/manual pidstat collector (legacy, see caveat below)
│   ├── vanilla/{cpu,ram}/*.csv   # Per-component CSVs, mode=vanilla (written by the sweep)
│   └── prototype/{cpu,ram}/*.csv # Per-component CSVs, mode=proto  (written by the sweep)
├── results/                      # Rate-sweep CSVs written by sweep_throughput_wrk2.py --out
├── plots/                        # PNG figures written by the two plotting scripts
└── README.md                     # This documentation file
```

---

## Prerequisites

### 1. Install wrk and wrk2 (client machine)
*   **wrk** (for concurrency sweeps):
    ```bash
    sudo apt update
    sudo apt install wrk -y
    ```
*   **wrk2** (for constant-throughput rate sweeps):
    The sweeper looks for the `wrk` binary inside `~/wrk2/` or the global `PATH`. Make sure it is
    compiled and accessible from wherever you run `sweep_throughput_wrk2.py`.

### 2. Python packages (client machine, for plotting)
```bash
pip3 install matplotlib numpy pandas
```

### 3. Active functions on the Pi
Ensure both Vanilla and Prototype SCM_RIGHTS functions are deployed:
*   **Vanilla functions**: `vanilla-fn-a`, `vanilla-fn-b`
*   **Prototype functions**: `sumprod-timing-fn-a`, `sumprod-timing-fn-b`

Deploy them with `faas-cli` from the main deploy directory:
```bash
faas-cli deploy -f deploy/vanilla-fn-a.yml
faas-cli deploy -f deploy/vanilla-fn-b.yml
faas-cli deploy -f deploy/sumprod-timing-fn-a.yml
faas-cli deploy -f deploy/sumprod-timing-fn-b.yml
```

### 4. Passwordless sudo on the Pi (required for per-component pidstat)
The sweep script SSHes into the Pi and runs `sudo ctr ...` / `sudo pidstat ...` **non-interactively**
to resolve PIDs and sample CPU/RAM. The Pi user (`romero`) must have NOPASSWD sudo configured
(e.g. via `visudo`, a line like `romero ALL=(ALL) NOPASSWD: ALL`). Verify with:
```bash
ssh romero@192.168.2.2 "sudo -n true && echo OK"
```
If this prints `OK` without prompting for a password, you're set. If pidstat collection isn't
needed, you can skip this and pass `--no-pidstat` to the sweep (see below).

---

## Part 1: wrk2 Rate Sweep (Recommended)

The **X-axis is the rate** (target requests/sec). For each rate step, the script measures actual
RPS, latency percentiles, Pi-wide CPU usage, **and** per-component CPU/RAM via `pidstat` —
all sampled over the exact same time window as that rate step (no drift into pause gaps).

### Step 1: Copy the Pinning Utility to the Pi
```bash
scp benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/pi_pin_all.sh romero@192.168.2.2:/tmp/
```

### Step 2: Pin the Server to Cores
For example, to pin to exactly **2 cores**:
```bash
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env NUM_CORES=2 bash /tmp/pi_pin_all.sh"
```

### Step 3: Run the Sweeps on the Client Machine

1. **Run Vanilla Rate Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/sweep_throughput_wrk2.py \
       --mode vanilla \
       --concurrency 100 \
       --payload-kb 1 \
       --rates 50,100,150,200,250,300,350,400,450,500,550,600,650,700 \
       --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/vanilla_https_rate_sweep_1kb_fixed_100concurence.csv
   ```

2. **Run SCM_RIGHTS Prototype Rate Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/sweep_throughput_wrk2.py \
       --mode proto \
       --concurrency 100 \
       --payload-kb 1 \
       --rates 50,100,150,200,250,300,350,400,450,500,550,600,650,700 \
       --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/proto_https_rate_sweep_1kb_fixed_100concurence.csv
   ```

Both runs also populate `pidstat/vanilla/` and `pidstat/prototype/` automatically (see Part 1bis).

#### All CLI options (`sweep_throughput_wrk2.py`)

| Flag             | Default                              | Meaning |
|------------------|---------------------------------------|---------|
| `--mode`         | *(required)* `vanilla` \| `proto`     | Which gateway/function set to hit |
| `--rates`        | `50,100,...,700`                      | Comma-separated target rates (req/s), one sweep step each |
| `--target-mode`  | `alternate`                           | `alternate` between fn-a/fn-b, or `same` fn for every request |
| `--concurrency`  | `100`                                 | Fixed wrk2 concurrent connections |
| `--payload-kb`   | `32`                                  | POST body size in KB |
| `--out`          | *(required)*                          | Output CSV path for the rate-sweep results table |
| `--pi-ssh`       | `romero@192.168.2.2`                  | SSH target for Pi-side CPU/pidstat sampling |
| `--duration-s`   | `20`                                  | wrk2 test duration per rate step (also the pidstat sampling window) |
| `--timeout-s`    | `10`                                  | wrk2 per-request timeout |
| `--threads`      | `4`                                   | wrk2 client threads |
| `--pause`        | `5`                                   | Pause (seconds) between rate steps |
| `--no-pidstat`   | off                                    | Disable per-component pidstat collection for this run |

---

## Part 1bis: Per-Component pidstat (integrated into the sweep)

For **each rate step**, `sweep_throughput_wrk2.py`:
1. Resolves, on the Pi, the PID of every component instance — `gateway`, `faasd` (its ~11
   processes are summed into one logical component), each function's `fwatchdog-<fn>`, and that
   fwatchdog's `worker-<fn>` child(ren). Functions sharing the same binary/comm string (e.g.
   `sumprod-timing-fn-a` and `-b` both run `timing-fn-ka-wo`) are still told apart because PIDs are
   resolved individually per function via `ctr -n openfaas-fn task ls`, never by comm-string alone.
2. Starts `pidstat -u` (CPU) and `pidstat -r` (RAM) on the Pi, restricted to exactly those PIDs,
   sampling once per second for `--duration-s` seconds — the same window as that rate step's wrk2
   run, so samples never drift into the `--pause` gap between steps.
3. Within each 1-second sample, sums same-label PIDs (e.g. faasd's processes, or two worker
   children of the same function) — this gives the correct combined load of that one logical
   component, while keeping fn-a and fn-b always separate.
4. Reduces the whole window's per-second sums to a single **mean** per component per metric, and
   appends one row (keyed by `rate`) to that component's CSV.

This produces, per mode, one CSV per component:

```
pidstat/<vanilla|prototype>/cpu/<component>.csv   → rate, usr_pct, system_pct, cpu_pct
pidstat/<vanilla|prototype>/ram/<component>.csv   → rate, minflt_s, majflt_s, vsz_kb, rss_kb, mem_pct
```

where `<component>` is one of `gateway`, `faasd`, `fwatchdog-<fn>`, `worker-<fn>` (e.g.
`fwatchdog-sumprod-timing-fn-a`, `worker-vanilla-fn-b`). All CSVs for a given mode have one row
per rate step, in the same order, so line N of `gateway.csv` and line N of `worker-<fn>.csv`
correspond to the same rate step.

To skip this (e.g. quick test run, or pidstat/sudo not available), pass `--no-pidstat`.

### Standalone alternative: `pidstat/collect_pidstat.sh` (legacy, manual)

Before the integration above existed, the same PID-resolution/summing/aggregation logic was run
as an **independent** script directly on the Pi, decoupled from the wrk2 timing:

```bash
sudo ./pidstat/collect_pidstat.sh --mode <vanilla|prototype> --duration <seconds> --interval <seconds>
```

*   `--duration` = total collection time (≈ number of rate steps × step duration)
*   `--interval` = aggregation window in seconds (≈ your wrk2 step duration)
*   Aggregates each window's per-second sums using the **mean**.
*   Output: same `pidstat/<mode>/{cpu,ram}/<component>.csv` layout, but **one row per fixed time
    window** (column `timestamp`) instead of one row per rate step (column `rate`).

**Caveat**: because this script runs on its own clock, its fixed windows are only as well
aligned to wrk2's rate steps as however precisely you start it relative to the sweep — any
misalignment lets a window land partly or fully inside a `--pause` gap, which can read as
artificially low/zero usage. **Prefer the integrated sweep (Part 1bis) for actual evaluation
runs**; this script is kept for manual/ad-hoc monitoring only.

---

## Part 2: Plotting

### A. Plotting `results/*.csv` (rate-sweep comparison) — "comme d'habitude"

`compare_two_csv_plots.py` compares two rate-sweep result CSVs (e.g. vanilla vs. proto) on a
shared `rate` x-axis and writes 6 PNG figures (grouped bar + line per metric, with socket-timeout
counts annotated above bars when present):

```bash
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/compare_two_csv_plots.py \
    benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/vanilla_https_rate_sweep_1kb_fixed_100concurence.csv \
    benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/proto_https_rate_sweep_1kb_fixed_100concurence.csv \
    --label-a Vanilla --label-b Prototype \
    --prefix https_1kb_100c \
    --out-dir benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plots/compare_https_1kb_100c
```

Required columns in each input CSV (already produced by `sweep_throughput_wrk2.py --out ...`):
`rate, rps, transfer_kb_s, pi_cpu_busy_avg_pct, pi_cpu_busy_max_pct, lat_avg_ms, total_requests, socket_timeout_errors`.

Generates, inside `--out-dir`, 6 figures named `<prefix>_<metric>.png`:
`throughput_kb_s`, `rps`, `cpu_avg_pct`, `cpu_max_pct`, `lat_avg_ms`, `total_requests`.

### B. Plotting `pidstat/*` (per-component CPU/RAM) — new, one mode at a time

`plot_pidstat.py` reads every CSV under `pidstat/<mode>/{cpu,ram}/`, and for each metric draws
one figure with **one line per component** across the rate sweep:

```bash
# Prototype mode
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plot_pidstat.py \
    --mode proto \
    --out-dir benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plots/pidstat_proto

# Vanilla mode
python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plot_pidstat.py \
    --mode vanilla \
    --out-dir benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plots/pidstat_vanilla
```

`--mode proto` reads from `pidstat/prototype/`, `--mode vanilla` reads from `pidstat/vanilla/`
(`--pidstat-dir` overrides this if you want to point at a different location). `--out-dir`
defaults to `plots/pidstat_<mode>/` if omitted.

Generates 5 figures per mode:
*   CPU: `cpu_usr_pct.png`, `cpu_system_pct.png`, `cpu_cpu_pct.png`
*   RAM: `ram_rss_kb.png`, `ram_mem_pct.png`

Each figure's legend lists every component found for that mode (e.g. `gateway`, `faasd`,
`fwatchdog-sumprod-timing-fn-a`, `worker-sumprod-timing-fn-b`, ...), so you can directly compare
which component dominates CPU/RAM usage as the rate increases.

---

## Part 3: Concurrency Sweeps using wrk (Alternative)

If you wish to sweep parallel connections (`concurrency`) instead of rate, to find max
sustainable throughput:

1. **Run Vanilla Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/sweep_throughput.py \
       32 2 vanilla \
       benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/vanilla_concurrency_2core_32kb.csv
   ```
2. **Run SCM_RIGHTS Prototype Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/sweep_throughput.py \
       32 2 proto \
       benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/proto_concurrency_2core_32kb.csv
   ```
   Positional args: `payload_kb`, `num_cores` (the Pi pinning used for that run — label only),
   `mode` (`vanilla`|`proto`), `output_csv`, and an optional `url` override.

3. **Plot Concurrency Sweep**:
   ```bash
   python3 benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plot_throughput.py \
       --vanilla benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/vanilla_concurrency_2core_32kb.csv \
       --proto benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/results/proto_concurrency_2core_32kb.csv \
       --out benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput/plots/concurrency_comparison_2core.png
   ```

---

## Quick End-to-End Recap

```bash
cd benchmarks/micro/micro-bench3-keepalive-https-integration/evaluation_throughput

# 1) Pin Pi cores (once per run)
ssh romero@192.168.2.2 "echo tchiaze2003 | sudo -S env NUM_CORES=2 bash /tmp/pi_pin_all.sh"

# 2) Run both sweeps (writes results/*.csv AND pidstat/{vanilla,prototype}/{cpu,ram}/*.csv)
python3 sweep_throughput_wrk2.py --mode vanilla --concurrency 100 --payload-kb 1 \
    --rates 50,100,200,400,800 --out results/vanilla_https_rate_sweep_1kb_fixed_100concurence.csv
python3 sweep_throughput_wrk2.py --mode proto --concurrency 100 --payload-kb 1 \
    --rates 50,100,200,400,800 --out results/proto_https_rate_sweep_1kb_fixed_100concurence.csv

# 3) Plot the rate-sweep comparison (6 figures)
python3 compare_two_csv_plots.py \
    results/vanilla_https_rate_sweep_1kb_fixed_100concurence.csv \
    results/proto_https_rate_sweep_1kb_fixed_100concurence.csv \
    --prefix https_1kb_100c --out-dir plots/compare_https_1kb_100c

# 4) Plot per-component CPU/RAM breakdown (5 figures per mode)
python3 plot_pidstat.py --mode vanilla --out-dir plots/pidstat_vanilla
python3 plot_pidstat.py --mode proto   --out-dir plots/pidstat_proto
```
