# Validation & Troubleshooting Guide

## Quick Checklist Before Running Tests

- [ ] Gateway binary exists: `ls libtlspeek/build/gateway`
- [ ] Worker binary exists: `ls libtlspeek/build/worker`
- [ ] wolfSSL library built: `file wolfssl/src/.libs/libwolfssl.so`
- [ ] Certificates exist: `ls libtlspeek/certs/server.crt libtlspeek/certs/server.key`
- [ ] wrk installed: `which wrk`
- [ ] nginx installed: `which nginx`
- [ ] Python 3 available: `python3 --version`
- [ ] File descriptor limits raised: `ulimit -n` should show 65535

If any fails:
```bash
# Rebuild gateway/worker
cd libtlspeek && mkdir -p build && cd build && cmake .. && make -j4 && cd ../..

# Rebuild wolfSSL if needed
cd wolfssl && make clean && ./autogen.sh && ./configure \
  --enable-tls13 --enable-aesgcm --enable-chacha --enable-hkdf \
  --enable-opensslextra --enable-keying-material --enable-debug && make -j4 && cd ..

# Install dependencies
sudo apt-get install -y nginx wrk python3-pip python3-pandas
```

---

## Running a Quick Validation Test

### Small Test (1 minute total)
```bash
cd Eval_perf

# Test all 3 modes with 10 workers, low concurrency
bash run_evaluation_fixed.sh 10 4 5 "50"

# Check results
tail -5 consolidated_results_fixed.csv
```

**Expected output:**
```
Mode,NumWorkers,Concurrency,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,MaxLatency_ms,...
nginx_direct,10,50,85000,...
nginx_proxy,10,50,80000,...
hotpotato,10,50,83000,...

CHECK: direct > proxy AND direct ≈ hotpotato
```

### Full Test (3 hours)
```bash
bash run_evaluation_fixed.sh 100 5 30 "100 500 1000 1500"
```

---

## Interpreting Warnings & Errors

### Error: "Gateway bind failed" or "Address already in use"
```bash
# Port still in use from previous run
lsof -i :8443  # Check gateway port
lsof -i :8445  # Check direct/proxy port
sudo kill -9 <PID>

# Or restart from clean state
sudo killall -9 gateway worker nginx direct_tls_server proxy_worker
sleep 3
bash run_evaluation_fixed.sh start nginx_direct 10
```

### Error: "wolfSSL library not found"
```bash
# Check LD_LIBRARY_PATH
echo $LD_LIBRARY_PATH
# Should include: .../wolfssl/src/.libs

# Fix:
export LD_LIBRARY_PATH=/path/to/wolfssl/src/.libs:$LD_LIBRARY_PATH
```

### Error: "wrk: Cannot resolve address"
```bash
# HTTPS hostname resolution issue
# Try direct IP:
wrk --latency -c 100 -d 5s https://127.0.0.1:8445/function/hello

# If SSL certificate error, use:
wrk --latency -c 100 -d 5s --insecure https://localhost:8445/function/hello
```

### Warning: "High CPU (>15%) in first test"
```bash
# Workers are still bottleneck, increase NUM_WORKERS
bash run_evaluation_fixed.sh 200 5 30 "100"  # Try 200 workers

# If still high, problem is in worker code or OS limits
ulimit -n  # Check FD limit (should be 65535)
```

---

## Live Monitoring During Test

Open separate terminal to watch progress:

### Monitor Processes
```bash
# Check all workers are running
while true; do
  echo "=== Worker counts ==="
  pgrep -c worker && echo " workers running"
  pgrep -c proxy_worker && echo " proxy workers running"
  pgrep -c nginx && echo " nginx running"
  pgrep -c direct_tls_server && echo " direct servers running"
  echo ""
  sleep 5
done
```

### Monitor CPU & Memory
```bash
# Real-time per-process monitoring
watch -n 1 'ps aux | grep -E "worker|gateway|nginx|direct_tls" | awk "{print \$3,\$6,\$11}" | head -20'

# Or use top
top -p $(pgrep -f "gateway|worker|nginx|direct_tls" | tr '\n' ',' | sed 's/,$//')
```

### Monitor Socket Usage
```bash
# Check Unix socket traffic
watch -n 1 'ls -la *.sock | wc -l'    # Number of sockets
ss -xa | grep -E "worker.*ESTAB"      # Active connections
```

---

## Analyzing Results

### Extract Key Metrics
```bash
python3 - << 'EOF'
import pandas as pd
df = pd.read_csv('consolidated_results_fixed.csv')

# Group by mode
for mode in df['Mode'].unique():
    mode_df = df[df['Mode'] == mode]
    avg_rps = mode_df['RPS'].mean()
    avg_lat = mode_df['AvgLatency_ms'].mean()
    avg_cpu = mode_df['AvgCPU_pct'].mean()
    print(f"{mode:15} {avg_rps:8.0f} RPS | {avg_lat:6.2f}ms | {avg_cpu:5.1f}% CPU")

# Overhead calculation
direct_rps = df[df['Mode'] == 'nginx_direct']['RPS'].iloc[0]
for mode in ['nginx_proxy', 'hotpotato']:
    mode_rps = df[df['Mode'] == mode]['RPS'].iloc[0]
    overhead = (direct_rps - mode_rps) / direct_rps * 100
    print(f"{mode} overhead: {overhead:+.1f}%")
EOF
```

### Generate Plots
```bash
python3 << 'EOF'
import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('consolidated_results_fixed.csv')

# Throughput comparison
fig, axes = plt.subplots(2, 2, figsize=(12, 8))

# RPS vs Concurrency
ax = axes[0, 0]
for mode in df['Mode'].unique():
    mode_df = df[df['Mode'] == mode].sort_values('Concurrency')
    ax.plot(mode_df['Concurrency'], mode_df['RPS'], marker='o', label=mode)
ax.set_xlabel('Concurrency')
ax.set_ylabel('RPS')
ax.set_title('Throughput Comparison')
ax.legend()
ax.grid()

# Latency vs Concurrency
ax = axes[0, 1]
for mode in df['Mode'].unique():
    mode_df = df[df['Mode'] == mode].sort_values('Concurrency')
    ax.plot(mode_df['Concurrency'], mode_df['AvgLatency_ms'], marker='o', label=mode)
ax.set_xlabel('Concurrency')
ax.set_ylabel('Avg Latency (ms)')
ax.set_title('Latency Comparison')
ax.legend()
ax.grid()

# CPU Usage
ax = axes[1, 0]
for mode in df['Mode'].unique():
    mode_df = df[df['Mode'] == mode].sort_values('Concurrency')
    ax.plot(mode_df['Concurrency'], mode_df['AvgCPU_pct'], marker='o', label=mode)
ax.set_xlabel('Concurrency')
ax.set_ylabel('Average CPU %')
ax.set_title('CPU Usage')
ax.legend()
ax.grid()

# P99 Latency
ax = axes[1, 1]
for mode in df['Mode'].unique():
    mode_df = df[df['Mode'] == mode].sort_values('Concurrency')
    ax.plot(mode_df['Concurrency'], mode_df['P99Latency_ms'], marker='o', label=mode)
ax.set_xlabel('Concurrency')
ax.set_ylabel('P99 Latency (ms)')
ax.set_title('Tail Latency')
ax.legend()
ax.grid()

plt.tight_layout()
plt.savefig('evaluation_comparison.png', dpi=150)
print("Saved: evaluation_comparison.png")
EOF
```

### Overhead Summary
```bash
python3 << 'EOF'
import pandas as pd

df = pd.read_csv('consolidated_results_fixed.csv')

print("\n" + "="*80)
print(" EVALUATION SUMMARY ".center(80, "="))
print("="*80)

direct = df[df['Mode'] == 'nginx_direct'].iloc[0]
nginx  = df[df['Mode'] == 'nginx_proxy'].iloc[0]
hot    = df[df['Mode'] == 'hotpotato'].iloc[0]

print(f"\nBASELINE (Direct Mode):")
print(f"  RPS:    {direct['RPS']:,.0f}")
print(f"  Latency: {direct['AvgLatency_ms']:.2f} ms (P99: {direct['P99Latency_ms']:.2f} ms)")
print(f"  CPU:     {direct['AvgCPU_pct']:.1f}%")

nginx_overhead = (direct['RPS'] - nginx['RPS']) / direct['RPS'] * 100
print(f"\nNGINX PROXY:")
print(f"  RPS:    {nginx['RPS']:,.0f} ({nginx_overhead:+.1f}% vs baseline)")
print(f"  Latency: {nginx['AvgLatency_ms']:.2f} ms")
print(f"  CPU:     {nginx['AvgCPU_pct']:.1f}%")
print(f"  ✓ CORRECT: overhead is positive (expected)" if nginx_overhead > 0 else "  ✗ ERROR: overhead is negative!")

hot_overhead = (direct['RPS'] - hot['RPS']) / direct['RPS'] * 100
print(f"\nHOT POTATO:")
print(f"  RPS:    {hot['RPS']:,.0f} ({hot_overhead:+.1f}% vs baseline)")
print(f"  Latency: {hot['AvgLatency_ms']:.2f} ms")
print(f"  CPU:     {hot['AvgCPU_pct']:.1f}%")
print(f"  ✓ GOOD: overhead is small" if 0 <= hot_overhead <= 5 else f"  ? CHECK: overhead is {hot_overhead:.1f}%")

print("\n" + "="*80)
EOF
```

---

## Common Issues & Fixes

| Issue | Symptom | Solution |
|-------|---------|----------|
| Workers bottleneck | All modes use high CPU (>15%) | Increase NUM_WORKERS → 200 or 500 |
| Unfair comparison | nginx_direct != 117K RPS | Check manage_server.sh uses NUM_WORKERS |
| Port conflicts | Cannot bind to 8445/8443 | `sudo lsof -i :8445 && sudo kill <PID>` |
| wolfSSL not found | "error while loading" | Set `export LD_LIBRARY_PATH=...wolfssl/src/.libs` |
| Nginx socket errors | Nginx can't connect to workers | Check worker socket permissions: `ls -la *.sock` |
| wrk hangs | Test doesn't complete | Check certificate is valid, use `--insecure` |
| CPU peaks at 100% | System saturated | Reduce concurrency or increase NUM_WORKERS |

---

## Validation Report Template

After running evaluation, fill in:

```markdown
# Evaluation Validation Report

## Configuration
- Workers per mode: ___
- CPUs per mode: ___
- Test duration: ___s
- Concurrency levels: ___

## Results

| Mode | RPS | Latency | CPU % | Overhead vs Direct |
|------|-----|---------|-------|-------------------|
| Direct | ___ | ___ | ___ | - (baseline) |
| Nginx Proxy | ___ | ___ | ___ | ___% |
| Hot Potato | ___ | ___ | ___ | ___% |

## Validation Checks

- [ ] Direct mode is FASTEST (highest RPS)
- [ ] Nginx shows 5-10% overhead (expected)
- [ ] Hot Potato shows <5% overhead (good!)
- [ ] All modes use similar CPU (<1% variance)
- [ ] No errors in any mode
- [ ] Latency scales reasonably with concurrency

## Issues Found (if any)

[List any problems or unexpected results]

## Conclusion

✓ Evaluation is VALID / ✗ Evaluation is BROKEN

Reason: ...
```

---

## When to Run Real Application Tests

✓ Run real app tests when:
- Baseline evaluation passes all checks
- Overhead metrics are stable and expected
- Code changes are minimal and focused
- You have time for long-running tests (hours)

✗ Don't run real app tests if:
- Baseline nginx is faster than direct (evaluation broken)
- CPU allocation is unfair (different workers per mode)
- Results are highly variable (>10% variance)
- You're debugging code issues (use baseline first)
