# 📊 Fair Evaluation Guide

## Quick Start

### 1. Build Requirements
```bash
cd libtlspeek/build
cmake ..
make -j4
cd ../..

# Verify binaries exist
ls libtlspeek/build/gateway libtlspeek/build/worker
```

### 2. Run Simple Test (5 minutes)
```bash
cd Eval_perf
chmod +x simple_test.sh manage_server.sh
bash simple_test.sh 10 5 50
```

**Arguments for simple_test.sh:**
```bash
bash simple_test.sh [NUM_WORKERS] [DURATION] [CONCURRENCY]
                         ↓           ↓             ↓
                    10 workers   5 seconds   50 connections
```

### 3. Run Full Evaluation (2-3 hours)
```bash
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"
```

**Arguments for run_evaluation_fixed.sh:**
```bash
bash run_evaluation_fixed.sh [NUM_WORKERS] [NUM_CPUS] [DURATION] [CONCURRENCY_LEVELS]
                                  ↓            ↓          ↓             ↓
                             100 workers  5 CPUs   30 seconds   100,500,1000 conc
```

---

## Understanding the Arguments

### `simple_test.sh 10 5 50`

| Argument | Meaning | Default |
|----------|---------|---------|
| `10` | Number of workers per mode | 10 |
| `5` | Test duration per mode (seconds) | 5 |
| `50` | Concurrent connections | 50 |

**Example:**
```bash
bash simple_test.sh 10 5 50
# Starts direct, nginx, hotpotato with 10 workers each
# Each test runs for 5 seconds with 50 concurrent connections
```

---

### `run_evaluation_fixed.sh 100 5 30 "100 500 1000"`

| Argument | Meaning | Default |
|----------|---------|---------|
| `100` | Workers per mode | 100 |
| `5` | CPUs per mode | 5 |
| `30` | Duration per test (seconds) | 30 |
| `"100 500 1000"` | Concurrency levels to test | "100 500 1000" |

**Example:**
```bash
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"
# Tests with: [direct, nginx, hotpotato] × [100, 500, 1000 connections]
# Total: 3 modes × 3 concurrency levels = 9 tests
# Time: ~3 × 30s = ~1.5 hours + overhead
```

---

## What Each Mode Tests

### Direct Mode (nginx_direct)
```
wrk ──HTTPS──> Port 8445 ──> 100 listeners (SO_REUSEPORT load-balanced)
                             └─> TLS 1.3 + HTTP response
Baseline: Should be FASTEST
```

### Nginx Proxy Mode (nginx_proxy)
```
wrk ──HTTPS──> Port 8445 ──> Nginx (TLS termination)
                             └─> 100 workers (Unix sockets)
                                 └─> HTTP response
Expected: 5-10% slower than direct (proxy overhead)
```

### Hot Potato Mode (hotpotato)
```
wrk ──HTTPS──> Port 8443 ──> Gateway (MSG_PEEK + inspect + route)
                             └─> 100 workers (fd transfer via SCM_RIGHTS)
                                 └─> TLS session continue + HTTP response
Expected: 0-5% slower than direct (routing overhead is minimal)
```

---

## Expected Results

```
NUM_WORKERS=100, NUM_CPUS=5, DURATION=30s

CONCURRENCY: 100
  Direct:     ~117000 RPS (BASELINE ✓)
  Nginx:      ~110000 RPS (-6.0% overhead - expected ✓)
  Hot Potato: ~114000 RPS (-2.6% overhead - good!)

CONCURRENCY: 500
  Direct:     ~116000 RPS (BASELINE ✓)
  Nginx:      ~109000 RPS (-6.1% overhead - consistent ✓)
  Hot Potato: ~113000 RPS (-2.6% overhead - consistent)

CONCURRENCY: 1000
  Direct:     ~115000 RPS (BASELINE ✓)
  Nginx:      ~108000 RPS (-6.1% overhead - stable ✓)
  Hot Potato: ~112000 RPS (-2.6% overhead - stable)

✓ VALID EVALUATION:
  - Direct is always fastest
  - Nginx overhead is consistent (~6%)
  - Hot Potato overhead is minimal (~2%)
```

---

## Troubleshooting

### wrk: unable to connect
```bash
# Check if server is listening
lsof -i :8445  # For direct/nginx
lsof -i :8443  # For hot potato

# Try connecting manually
curl -k https://127.0.0.1:8445/function/hello
```

### "Address already in use"
```bash
# Kill leftover processes
sudo pkill -9 -f "gateway|worker|nginx|direct_tls_server|proxy_worker"
sleep 3

# Check again
lsof -i :8443 :8445
```

### Gateway crashes or "MAX_WORKERS"
```bash
# Rebuild gateway with updated MAX_WORKERS
cd libtlspeek/build
cmake .. && make -j4
```

### No output from wrk
```bash
# Test with plain URL to debug
wrk -t 2 -c 10 -d 2s https://127.0.0.1:8445/ 2>&1 | head -50

# If that fails, check server is running
curl -v https://127.0.0.1:8445/function/hello 2>&1 | head -20
```

---

## Output Files

After running `simple_test.sh`:
```
direct_bench.txt       ← Direct mode wrk results
nginx_bench.txt        ← Nginx mode wrk results
hotpotato_bench.txt    ← Hot Potato mode wrk results
direct.log             ← Direct server debug log
nginx.log              ← Nginx debug log
gateway.log            ← Gateway debug log
worker_*.log           ← Per-worker debug logs
```

After running `run_evaluation_fixed.sh`:
```
consolidated_results_fixed.csv  ← CSV with all metrics
wrk_*.txt                       ← Per-test wrk outputs
cpu_monitor_*.log               ← CPU usage during tests
```

---

## Interpreting CSV Results

```
Mode,NumWorkers,Concurrency,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,...

Key columns:
  - Mode: nginx_direct, nginx_proxy, or hotpotato
  - RPS: Requests per second (higher is better)
  - AvgLatency_ms: Average response time
  - P99Latency_ms: 99th percentile latency
  - AvgCPU_pct: CPU percentage used
```

**Analysis:**
```bash
# Extract key metrics
grep "^nginx_direct" consolidated_results_fixed.csv | cut -d',' -f1,3,4,6
# Mode, Concurrency, RPS, AvgLatency

# Calculate overhead
python3 << 'EOF'
import pandas as pd
df = pd.read_csv('consolidated_results_fixed.csv')

for conc in df['Concurrency'].unique():
    direct = df[(df['Mode']=='nginx_direct') & (df['Concurrency']==conc)]['RPS'].values[0]
    nginx = df[(df['Mode']=='nginx_proxy') & (df['Concurrency']==conc)]['RPS'].values[0]
    hotpot = df[(df['Mode']=='hotpotato') & (df['Concurrency']==conc)]['RPS'].values[0]
    
    print(f"Concurrency {conc}:")
    print(f"  Direct: {direct:.0f} RPS")
    print(f"  Nginx:  {nginx:.0f} RPS ({(direct-nginx)/direct*100:+.1f}%)")
    print(f"  HotPot: {hotpot:.0f} RPS ({(direct-hotpot)/direct*100:+.1f}%)")
EOF
```

---

## Next Steps

1. **Verify basic setup** → Run `simple_test.sh 10 5 50`
2. **Check results** → All modes should complete without errors
3. **Run full test** → `run_evaluation_fixed.sh 100 5 30 "100 500 1000"`
4. **Analyze** → Check CSV, plot curves, validate overhead metrics
5. **Real workload** → Modify handler to do real work, re-run evaluation

---

## Tips for Production-Quality Results

1. **Disable CPU scaling**: `sudo cpupower frequency-set -g performance`
2. **Avoid background load**: Kill other processes, dedicated testing machine
3. **Multiple runs**: Run evaluation 2-3 times, average the results
4. **Warm-up phase**: Add 60s warm-up before collecting results
5. **Increase duration**: Use 60s+ per test for stability

---

**Questions? Check the logs!**
```bash
# Full debug output
tail -100 gateway.log direct.log nginx.log worker_*.log
```
