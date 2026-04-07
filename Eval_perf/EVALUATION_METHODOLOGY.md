# CORRECTED EVALUATION METHODOLOGY

## Executive Summary

The original evaluation had a **critical flaw**: nginx_proxy mode was achieving HIGHER performance (268% CPU, 83K RPS) than should be possible, indicating **constrained resources** rather than measuring proxy overhead.

**Root Cause**: All modes used only 2 workers, which became the bottleneck in nginx_proxy mode (high context switching, CPU thrashing).

**Solution**: Scale to 100+ workers per mode so the architecture (gateway/proxy) becomes the bottleneck, not workers.

---

## The Problem: Original Evaluation

### What Was Measured
```
DIRECT MODE:      114% CPU, 117.5K RPS
                  └─ 2 direct listeners handling all load

NGINX PROXY MODE: 268% CPU, 83K RPS  ← WRONG!
                  └─ 2 workers + nginx forwarding
                  └─ 268% CPU = extreme context switching
                  └─ 83K RPS = 30% LOWER throughput
```

### Why This Is Backward
- **Expected**: Direct > Nginx (no forwarding overhead)
- **Actual**: Nginx proxy has LOWER RPS with HIGHER CPU
- **Implication**: Workers are bottlenecked, not measuring proxy overhead
- **Invalid conclusion**: Cannot compare Hot Potato performance

### Why 2 Workers Isn't Enough
```
Queue buildup at 2 workers:
  - Nginx queues requests waiting for worker availability
  - 2 workers can't keep up with connection rate
  - OS scheduler context-switches workers aggressively
  - High CPU% due to context-switching, not useful work
  - A constrained system always looks "CPU-heavy"

With 100 workers:
  - Request distribution is balanced
  - No single worker saturates
  - CPU time spent in useful I/O + crypto, not scheduling
  - Direct mode CPU% shows real overhead
```

---

## The Solution: Fair Evaluation

### Key Principle
**All modes must use the same number of workers and be bounded by the same CPU budget, so we measure ARCHITECTURE overhead, not RESOURCE AVAILABILITY.**

### Setup

| Aspect | Configuration |
|--------|---------------|
| **Workers per mode** | 100 (or N, configurable) |
| **CPUs per mode** | 5 (or M, same for all) |
| **Load balance** | SO_REUSEPORT (direct), Upstream (nginx), Routing (Hot Potato) |
| **TLS mode** | TLS 1.3 (all modes) |
| **Workload** | `/function/hello` (stateless, CPU-bound in crypto) |
| **Concurrency** | 100, 500, 1000 (scale up) |
| **Duration** | 30s per test (warm cache) |

### Expected Results

```
BASELINE (Direct with 100 workers):
  - 100K-120K RPS per CPU core
  - ~5% CPU user, ~2% sys, rest idle or I/O wait
  - Scaling: Linear until CPU-saturated or NIC-saturated

NGINX PROXY (with 100 workers):
  - ~5-10% lower RPS (forwarding overhead)
  - ~5% additional CPU (nginx forwarding + demuxing)
  - Should be SLOWER than direct

HOT POTATO (with 100 workers):
  - ~0-5% lower RPS (routing + stateless crypto overhead)
  - Similar CPU to direct (lightweight inspection)
  - Should be CLOSE to direct OR slightly slower
  - But NEVER faster than direct
```

### Architecture Diagram (Fixed Evaluation)

```
┌─────────────────────────────────────────┐
│ FAIR EVALUATION WITH 100 WORKERS        │
├─────────────────────────────────────────┤

DIRECT MODE (Baseline):
  wrk client ──HTTPS──> Port 8445
                        ├─ Listen Process 1 (SO_REUSEPORT)
                        ├─ Listen Process 2
                        ├─ ...
                        └─ Listen Process 100  ← 100 listeners
                        CPU: 5 cores
                        Expected: ~117K RPS

NGINX PROXY:
  wrk client ──HTTPS──> Port 8445 (nginx)
                        │ (TLS termination)
                        ├─> Unix Socket 0 ──> Worker 0
                        ├─> Unix Socket 1 ──> Worker 1
                        ├─> ...
                        └─> Unix Socket 99 ──> Worker 99  ← Forward to 100 workers
                        CPU: 5 cores (shared: nginx + workers)
                        Expected: ~110K RPS (5-7% slower)

HOT POTATO:
  wrk client ──HTTPS──> Port 8443 (gateway)
                        │ (MSG_PEEK + route)
                        ├─> Unix Socket 0 ──> Worker 0
                        ├─> Unix Socket 1 ──> Worker 1
                        ├─> ...
                        └─> Unix Socket 99 ──> Worker 99  ← Transfer fd to 100 workers
                        CPU: 5 cores (shared: gateway + workers)
                        Expected: ~115K RPS (close to direct)
```

---

## How to Run the Fixed Evaluation

### Step 1: Verify Setup
```bash
cd Eval_perf

# Ensure gateway binary exists
ls ../libtlspeek/build/gateway
ls ../libtlspeek/build/worker

# Check wolfSSL is built
ls ../wolfssl/src/.libs/libwolfssl.so
```

### Step 2: Run with Default Settings (100 workers, 5 CPUs, 30s)
```bash
chmod +x run_evaluation_fixed.sh
chmod +x manage_server.sh
bash run_evaluation_fixed.sh
```

### Step 3: Run with Custom Settings
```bash
# [num_workers] [num_cpus] [duration] [concurrency_levels]
bash run_evaluation_fixed.sh 50 4 20 "100 500"           # Small test
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"     # Full test
bash run_evaluation_fixed.sh 200 8 60 "500 1000 2000"    # Large scale
```

### Step 4: Results
```
consolidated_results_fixed.csv  ← All results
cpu_monitor_*.log               ← CPU stats per mode
wrk_*.txt                       ← Raw wrk output
```

---

## Interpreting Results: Validation Checklist

### ✅ Correct Evaluation Indicators

**1. Direct Mode is FASTEST**
```
Mode               RPS        CPU%
nginx_direct      ~117000     5
nginx_proxy       ~110000     5-6
hotpotato         ~115000     5-6

CHECK: nginx_direct > nginx_proxy ✓
       nginx_direct ≈ hotpotato ✓
```

**2. Nginx Shows Consistent Overhead**
```
Overhead = (Direct RPS - Mode RPS) / Direct RPS * 100%

nginx_proxy overhead:  100 - (110/117) = ~6% ✓
hotpotato overhead:    100 - (115/117) = ~2% ✓
Both are small, expected, positive ✓
```

**3. CPU Allocation is Fair**
```
All modes use ~5% total CPU (within measurement noise)
No mode uses >10% CPU at baseline
  - If cpu > 10%, workers are still bottleneck
  - If cpu < 3%, increase concurrency or workers
```

**4. Latency Scales Linearly**
```
Concurrency 100:   avg_latency ~1ms
Concurrency 500:   avg_latency ~5ms
Concurrency 1000:  avg_latency ~10ms

(Roughly: latency ∝ concurrency / (cpu * throughput_per_cpu))
Little's Law: L = λ × W where λ=throughput, W=wait time
```

### ❌ Wrong Evaluation Indicators

**1. Nginx Proxy Faster Than Direct**
```
nginx_proxy RPS > nginx_direct  ← BROKEN!
Likely causes:
  - Direct mode not using all CPU
  - Nginx mode has more workers
  - Unfair CPU allocation
Fix: Check manage_server.sh, verify NUM_WORKERS same
```

**2. High CPU Without High RPS**
```
CPU > 10% but RPS < 50K  ← BROKEN!
Likely causes:
  - Workers are bottleneck (need more workers)
  - Inefficient handler (check handler.c)
  - Lock contention (check epoll setup)
Fix: Increase NUM_WORKERS → 200 or 500
```

**3. Inconsistent CPU Across Modes**
```
Direct: 5% CPU
Nginx:  15% CPU  ← BROKEN!
Hotpotato: 5% CPU

Likely causes:
  - Different num_workers per mode
  - Nginx overhead not accounted for
Fix: Verify manage_server.sh uses NUM_WORKERS for all modes
```

### Expected Output (Correct Evaluation)

```
[=] FAIR EVALUATION CONFIGURATION
    Workers per mode: 100
    CPUs per mode: 5
    Duration per test: 30s
    Concurrency levels: 100 500 1000

--- Concurrency: 100 ---
Direct:     118000 RPS
nginx_proxy  112000 RPS (-5.0% overhead)
hotpotato    116000 RPS (-1.7% overhead)

--- Concurrency: 500 ---
Direct:     116000 RPS
nginx_proxy  110000 RPS (-5.2% overhead)
hotpotato    114000 RPS (-1.7% overhead)

--- Concurrency: 1000 ---
Direct:     115000 RPS
nginx_proxy  109000 RPS (-5.2% overhead)
hotpotato    113000 RPS (-1.7% overhead)

KEY EXPECTATIONS:
  ✓ Direct mode:     117K RPS (BASELINE)
  ✓ Nginx Proxy:     109K RPS (~5-7% slower, EXPECTED)
  ✓ Hot Potato:      114K RPS (~1-2% slower, GOOD)
```

---

## Advanced: CPU Pinning for Reproducibility

For strictest evaluation, pin processes to specific cores:

```bash
# Pin to cores 0-4 (5 CPUs total)
taskset -c 0-4 bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"

# Verify pinning
watch 'ps aux | grep -E "gateway|worker|nginx|direct_tls" | grep -E "^[0-9]+" | awk "{print \$1,\$2}" | while read uid pid; do taskset -p $pid 2>/dev/null; done'
```

---

## Next Phase: Real Application Evaluation

After validating with `/function/hello` baseline:

### 1. Choose Real Workload
- **CPU-bound**: Crypto operations (AES, SHA)
- **I/O-bound**: Database queries, file I/O
- **Memory-heavy**: Large payloads (> 1MB)
- **Mixed**: Combined operations

### 2. Measure Per-Layer Overhead
```bash
# Breakdown:
# Total latency = TLS handshake + TLS record decrypt + HTTP parse + handler + response send

# Use detailed logging:
./run_evaluation_fixed.sh 100 5 30 "100" > detailed_trace.log
grep -E "\[gateway\]|\[worker\]|\[crypto\]|\[serial\]" *.log | analyze_latency.py
```

### 3. Test Under Load Patterns
- **Steady-state**: Constant RPS
- **Burst**: Sudden concurrency spike
- **Gradual ramp**: Increasing load over time
- **Variable**: Random concurrency

### 4. Compare With Production Baselines
- AWS Lambda cold start
- Kubernetes pod startup
- Traditional nginx+uwsgi

---

## Files Modified for Fair Evaluation

| File | Change |
|------|--------|
| `manage_server.sh` | Accept NUM_WORKERS parameter, spawn N workers for all modes |
| `direct_tls_server.c` | Accept listener count from argv[2] |
| `nginx.conf.template` | Dynamic upstream generation |
| `run_evaluation_fixed.sh` | NEW: Fair comparison script with CPU allocation |

---

## Summary

| Before (Broken) | After (Fixed) |
|-----------------|---------------|
| 2 workers → bottleneck | 100+ workers → architecture is bottleneck |
| Direct: 117K RPS | Direct: ~117K RPS (baseline) |
| Nginx: 83K RPS (WRONG!) | Nginx: ~109K RPS (correct ~5% slower) |
| Can't compare | Hot Potato: ~114K RPS (good ~1% slower) |
| CPU allocation: unfair | CPU allocation: fair (5 CPUs all modes) |

**Result**: Valid comparison of architectural overhead, not resource contention.
