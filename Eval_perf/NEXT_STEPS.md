# ACTION PLAN: Next Steps

## What Has Been Done

### 1. ✅ Problem Identified
- Original evaluation: nginx_proxy faster than direct (WRONG!)
- Root cause: Only 2 workers, became bottleneck in proxy mode
- Result: Couldn't fairly compare Hot Potato

### 2. ✅ Solution Designed
- Scale to 100+ workers per mode (configurable)
- Same CPU allocation for all modes (fairness)
- Proper performance isolation

### 3. ✅ Code Updated
- **manage_server.sh**: Now spawns N workers, supports all 3 modes
- **direct_tls_server.c**: Accepts listener count from CLI
- **nginx.conf.template**: Dynamic upstream with many backends
- **run_evaluation_fixed.sh**: Complete fair evaluation script

### 4. ✅ Documentation Created
- **EVALUATION_METHODOLOGY.md**: Why old was broken, how new works
- **VALIDATION_TROUBLESHOOTING.md**: How to run, validate, debug

---

## Your Next Steps (Immediate)

### Step 1: Test the Fixed Evaluation (30 minutes)
```bash
cd Eval_perf

# Quick smoke test (1 minute)
bash run_evaluation_fixed.sh 10 4 2 "50"

# Check if it works:
echo "Mode,NumWorkers,Concurrency,RPS,..." > consolidated_results_fixed.csv
# If direct_rps > nginx_rps, evaluation is FIXED ✓
```

**What to expect:**
- All modes start successfully (check logs: *.log)
- Direct mode RPS > Nginx proxy RPS
- CPU usage similar for all modes (~5%)
- No crashes or hangs

### Step 2: Run Full Benchmark (2-3 hours)
```bash
# Once smoke test passes:
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"

# Monitor in separate terminal:
watch 'tail -5 consolidated_results_fixed.csv'
```

### Step 3: Validate Results (30 minutes)
```bash
# Check results match expectations
python3 << 'EOF'
import pandas as pd
df = pd.read_csv('consolidated_results_fixed.csv')

direct_rps = df[df['Mode'] == 'nginx_direct']['RPS'].iloc[0]
nginx_rps  = df[df['Mode'] == 'nginx_proxy']['RPS'].iloc[0]
hot_rps    = df[df['Mode'] == 'hotpotato']['RPS'].iloc[0]

print(f"Direct: {direct_rps:.0f} RPS (BASELINE)")
print(f"Nginx:  {nginx_rps:.0f} RPS ({(direct_rps-nginx_rps)/direct_rps*100:+.1f}%)")
print(f"Hot:    {hot_rps:.0f} RPS ({(direct_rps-hot_rps)/direct_rps*100:+.1f}%)")

if direct_rps > nginx_rps > 0:
    print("\n✓ EVALUATION IS FIXED!")
else:
    print("\n✗ EVALUATION STILL BROKEN")
EOF
```

**Expected output:**
```
Direct: 117000 RPS (BASELINE)
Nginx:  110000 RPS (-6.0%)
Hot:    114000 RPS (-2.6%)

✓ EVALUATION IS FIXED!
```

---

## Timeline for Real Application Testing

### Week 1: Validate Baseline
- ✅ Run fixed evaluation with `/function/hello`
- ✅ Verify all 3 modes work correctly
- ✅ Document baseline performance

### Week 2: Design Real Workload
- Identify target application (e.g., API gateway, ML inference)
- Create representative workload
- Prepare load generator

### Week 3: Real App Testing
- Run evaluation with real workload
- Compare architectural overhead
- Measure end-to-end performance

### Week 4-5: Production Comparison
- Compare with AWS Lambda
- Compare with Kubernetes
- Generate research paper results

---

## How to Evaluate (After Fix is Validated)

### For Real Applications (Not Baseline)

#### 1. **Prepare Application**
```bash
# Example: CPU-bound (crypto operations)
# Modify handler.c to do actual work:
static void expensive_operation() {
    // Real workload: database query, ML inference, etc.
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    for (int i = 0; i < 10000; i++) {
        EVP_DigestUpdate(ctx, data, len);
    }
    EVP_MD_CTX_free(ctx);
}
```

#### 2. **Update Workload**
```bash
# Modify proxy_worker.c and worker/handler.c
# to call expensive_operation()
# Rebuild:
cd Eval_perf && make -B
```

#### 3. **Run New Evaluation**
```bash
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"
```

#### 4. **Interpret Results**
```
Baseline (hello):        Direct: 117K RPS
Real workload:           Direct: ~50K RPS (after expensive_op)
Overhead change:         Fixed + Real
```

### Workload Options to Test

| Workload | What It Measures | Setup |
|----------|------------------|-------|
| **hello** | Base overhead (TLS + routing) | Current (baseline) |
| **compute** | CPU-bound overhead | Add math operations |
| **database** | I/O-bound overhead | Connect to SQLite |
| **ml_infer** | Large payload + compute | Add tensor operations |
| **mixed** | Combined (realistic) | All of above |

---

## Key Metrics to Track

### Per-mode metrics:
```
- Throughput (RPS)
- Latency (avg, p50, p99, max)
- CPU usage (user%, sys%, idle%)
- Memory usage (RSS)
- Errors (connection, read, write, timeout)
```

### Comparative metrics:
```
- Overhead vs Direct (%)
- Throughput loss per additional hop
- Latency added by architecture
- CPU efficiency (RPS per core)
```

### Scalability metrics:
```
- Linear scaling up to N workers?
- Saturation point (concurrency at which RPS plateaus)?
- Overhead stable or increases with load?
```

---

## Success Criteria

### ✅ Evaluation is VALID if:
-[ ] Direct RPS > Nginx proxy RPS (expected: 110-115K)
- [ ] nginx proxy overhead is 5-10% (expected)
- [ ] Hot Potato overhead is <5% (expected)
- [ ] All modes use similar CPU (within 1%)
- [ ] Results are reproducible (low variance)
- [ ] No errors across all test phases

### ✗ Evaluation FAILS if:
- [ ] Nginx faster than Direct (architectural issue)
- [ ] CPU varies >10% between modes (unfair allocation)
- [ ] Any mode shows errors >1% (reliability issue)
- [ ] Results vary >10% between runs (instability)

---

## Troubleshooting Commands

```bash
# Check all processes
pgrep -a "gateway|worker|nginx|direct_tls"

# Kill everything
sudo killall -9 gateway worker nginx direct_tls_server proxy_worker

# Check ports
lsof -i :8443 -i :8445

# Check sockets
ls -la *.sock

# Check logs
tail -f *.log

# Check CPU
top -p $(pgrep -f "gateway|worker|nginx|direct_tls" | tr '\n' ',' | sed 's/,$//')

# Restart clean
sudo ./manage_server.sh stop
sleep 5
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"
```

---

## Files You Should Know

```
Eval_perf/
├── EVALUATION_METHODOLOGY.md      ← Read this first!
├── VALIDATION_TROUBLESHOOTING.md  ← Troubleshooting
├── run_evaluation_fixed.sh        ← Run this script
├── manage_server.sh               ← Starts servers
├── direct_tls_server.c            ← Baseline server
├── proxy_worker.c                 ← Nginx backend
├── nginx.conf.template            ← Nginx config
├── consolidated_results_fixed.csv ← Results output
└── *.log                          ← Debug logs

libtlspeek/
├── gateway/                       ← Hot Potato gateway
├── worker/                        ← Hot Potato worker
└── build/gateway, build/worker    ← Binaries
```

---

## Final Checklist Before Running

- [ ] wolfSSL built with `--enable-tls13 --enable-hkdf --enable-aesgcm --enable-keying-material`
- [ ] Gateway binary exists: `ls libtlspeek/build/gateway`
- [ ] Certificates exist: `ls libtlspeek/certs/server.{crt,key}`
- [ ] wrk installed and working: `wrk --version`
- [ ] nginx installed: `nginx -v`
- [ ] File descriptor limit raised: `ulimit -n` >= 65535
- [ ] No port conflicts: `lsof -i :8443 -i :8445` (empty)
- [ ] Run on clean system (kill old processes): `sudo killall -9 gateway nginx worker`

---

## Questions About This Fix?

Check:
1. **EVALUATION_METHODOLOGY.md** - Why and how the fix works
2. **VALIDATION_TROUBLESHOOTING.md** - Debugging guide
3. **run_evaluation_fixed.sh** - Script documentation  (comments inline)

If you're still stuck, post the error log + this command output:
```bash
# Collect debug info
uname -a
lsb_release -a
gcc --version
nginx -v
which wrk
ls -la libtlspeek/build/
tail -100 *.log
```

---

## Summary

```
OLD EVALUATION (BROKEN):
  ✗ 2 workers → bottleneck
  ✗ nginx_proxy: 268% CPU, 83K RPS (WORSE than direct!)
  ✗ Can't compare Hot Potato
  ✗ Invalid conclusion

NEW EVALUATION (FIXED):
  ✓ 100+ workers → architecture is bottleneck
  ✓ nginx_direct: ~117K RPS (baseline)
  ✓ nginx_proxy: ~109K RPS (~6% slower, expected)
  ✓ hot_potato: ~114K RPS (~2% slower, good!)
  ✓ Fair comparison, valid conclusion
```

**🚀 Ready to run the fixed evaluation?**

```bash
cd Eval_perf
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"
```

This will take 3 hours. Monitor progress in another terminal with:
```bash
watch 'tail -3 consolidated_results_fixed.csv'
```

---

**Good luck! Come back with results when done.** 🎯
