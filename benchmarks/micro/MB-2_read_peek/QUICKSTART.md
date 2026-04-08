# MB-2 Evaluation - Complete Setup Summary

## ✅ What's Been Created

The MB-2 benchmark is **fully implemented** and ready to run.

### 📁 Files Created

```
benchmarks/micro/MB-2_read_peek/
├── README.md                    (12 KB) - Complete documentation
├── evaluate.sh                  (9 KB) - ONE-COMMAND execution
├── full_sync_to_pi.sh          (4 KB) - Project syncer
├── setup_and_compile.sh        (5 KB) - Builder
├── run_benchmark.sh            (5 KB) - Benchmark executor
├── plot_mb2.py                 (7 KB) - Visualization
├── mb2_server_pi.c             (13 KB) - Server source
└── mb2_client_remote.c         (18 KB) - Client source
```

---

## 📊 MB-2 Benchmark Overview

**Goal:** Measure the overhead of `tls_read_peek()` compared to just `wolfSSL_read()`

**Why:** libtlspeek needs to peek at data before returning it = decrypt twice = overhead

### Test Matrix

| Config | Operation | Cost |
|--------|-----------|------|
| **A**  | `wolfSSL_read()` only | Baseline |
| **B**  | `tls_read_peek()` + `wolfSSL_read()` | A + peek cost |

### Payload Sizes

- **256 B** - Small
- **1 KiB** - Medium  
- **4 KiB** - Large

### Expected Result

```
Overhead should be ~100% across all sizes
(peek adds roughly one full decryption)
```

**Why 100%?**
```
Config A: Decrypt once
Config B: Decrypt (peek) + Decrypt (read) = 2× decrypt
Overhead = 2× - 1× = 1× (100%)
```

---

## 🚀 QUICK START - ONE COMMAND

```bash
cd ~/Prototype_sendfd/benchmarks/micro/MB-2_read_peek
chmod +x evaluate.sh
./evaluate.sh
```

**That's it!** The script handles:
1. ✅ Sync to Pi
2. ✅ Build on Pi
3. ✅ Compile locally
4. ✅ Run benchmarks (40,000+ measurements)
5. ✅ Generate plots
6. ✅ Save results

**Time:** 60-120 minutes (first run)

---

## 📋 All Execution Steps (If you prefer manual)

### Step 1: Sync Project
```bash
bash full_sync_to_pi.sh romero 192.168.2.2
# Time: 5-30 min
```

### Step 2: Compile Server (Pi terminal)
```bash
ssh romero@192.168.2.2
cd ~/Prototype_sendfd/benchmarks/micro/MB-2_read_peek
bash setup_and_compile.sh pi
# Time: 5-10 min (builds wolfSSL if needed)
```

### Step 3: Compile Client (Local)
```bash
bash setup_and_compile.sh local
# Time: 1-2 min
```

### Step 4: Start Server (Pi, keep running)
```bash
export LD_LIBRARY_PATH=$HOME/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH
./mb2_server_pi
# Output: [SERVER] Listening on 0.0.0.0:19446
```

### Step 5: Run Benchmarks (Local, new terminal)
```bash
bash run_benchmark.sh 192.168.2.2
# Time: 30-45 min
```

### Step 6: Generate Plot
```bash
python3 plot_mb2.py --csv results_mb2_distributed_*.csv --no-display
# Creates: plot_mb2.png and plot_mb2.pdf
```

---

## 📖 What the Benchmark Measures

### Client-Side Measurement (mb2_client_remote.c)

```c
// For each payload size (256B, 1KiB, 4KiB):

// Measure Config A: Just read
Time_A = Time to run 1000× wolfSSL_read(payload_size)

// Measure Config B: Peek then read
Time_B = Time to run 1000× {
    tls_read_peek();        // Decrypt once
    wolfSSL_read();         // Decrypt again
}

// Overhead
Overhead = (Time_B - Time_A) / Time_A × 100
```

### Server-Side (mb2_server_pi.c)

```c
// Simply sends test payloads through TLS connection
for each payload_size in {256B, 1KiB, 4KiB}:
    for 100 iterations:
        Send payload_size bytes through SSL
```

---

## 📊 Understanding Results

### CSV Output Example

```csv
payload_size_bytes,config,iterations,total_time_us,avg_time_per_iteration_us,stddev_us,failed_count
256,A,1000,102000.00,102.00,5.23,0
256,B,1000,205000.00,205.00,8.15,0
1024,A,1000,358200.00,358.20,12.45,0
1024,B,1000,681500.00,681.50,19.83,0
4096,A,1000,1232000.00,1232.00,45.20,0
4096,B,1000,2456000.00,2456.00,67.50,0
```

### Calculate Overhead

```
Overhead_256B = (205.00 - 102.00) / 102.00 × 100 = 101%  ✓ Expected!
Overhead_1KB  = (681.50 - 358.20) / 358.20 × 100 = 90%   ~ Expected
Overhead_4KB  = (2456.00 - 1232.00) / 1232.00 × 100 = 99% ✓ Expected!
```

### Generated Plot

The `plot_mb2.png` will show:
```
Overhead (µs)
    │
 1200 │     ●
      │    /
  700 │   /  ●
      │  /
  200 │●/
      │
    └─────────────────→ Payload Size
     256B  1KiB  4KiB
```

**Key Feature:** Line should be roughly linear (overhead ∝ payload size)

---

## ⚡ Troubleshooting Quick Reference

### "Cannot connect to Pi"
```bash
ping -c 3 192.168.2.2
ssh romero@192.168.2.2 "echo OK"
```

### "Server won't compile"
```bash
# Check if wolfSSL is built
ssh romero@192.168.2.2 ls ~/Prototype_sendfd/wolfssl/src/.libs/libwolfssl.*
```

### "Client won't compile"
```bash
ls ~/Prototype_sendfd/wolfssl/src/.libs/libwolfssl.*
```

### "Benchmarks timeout"
```bash
# Check server is actually running
ssh romero@192.168.2.2 ps aux | grep mb2_server
```

---

## 🎯 What You'll See During Execution

### Server Terminal (Pi)
```
╔════════════════════════════════════════════════════════════╗
║  MB-2 SERVER - read_peek() overhead measurement           ║
╚════════════════════════════════════════════════════════════╝

[SERVER] Initializing wolfSSL...
[SERVER] Creating TLS server context...
[SERVER] Loading certificate...
[SERVER] Listening on 0.0.0.0:19446
[SERVER] Waiting for client connection...

[SERVER] Handling client from 192.168.2.X:XXXXX
[SERVER] Starting payload transmission...
  Sending 256B payloads...
  Sending 1KiB payloads...
  Sending 4KiB payloads...
✓ All payloads sent
```

### Client Terminal (Local)
```
╔════════════════════════════════════════════════════════════╗
║  MB-2 BENCHMARK - read_peek() vs wolfSSL_read() overhead  ║
╚════════════════════════════════════════════════════════════╝

Testing payload size: 256B
  ✓ Connected to server
  Measuring Config A...
    [100/1000]
    [200/1000]
    ...
    [1000/1000]
  ✓ Config A complete
  Measuring Config B...
    [100/1000]
    ...
  ✓ Config B complete

256,A,1000,102000.00,102.00,5.23,0
256,B,1000,205000.00,205.00,8.15,0

>>> Results for 256B:
    Config A (read only):    102.00 ± 5.23 µs
    Config B (peek + read):  205.00 ± 8.15 µs
    Overhead:                103.00 µs (+101.0%)

[✓] Benchmark complete. Results above in CSV format.
```

---

## 📈 Expected Results Analysis

### Interpretation Guide

| Overhead | Status | Meaning |
|----------|--------|---------|
| 80-120% | ✅ Perfect | Peek costs exactly one decryption |
| 50-80% | ⚠️ Acceptable | Some optimization happening |
| 0-50% | ❌ Unexpected | Peek might be skipped/cached |
| >150% | ❌ Unexpected | Inefficient implementation |

### Key Properties

1. **Overhead should be ~100%** - One extra decrypt per peek
2. **Should grow with payload** - More data = longer decrypt
3. **Should be consistent** - ±10% variation is normal
4. **Error bars may overlap** - Within measurement noise

---

## 🔍 MB-2 vs MB-1

| Aspect | MB-1 | MB-2 |
|--------|------|------|
| **Measures** | Keylog callback overhead | Double-decrypt overhead |
| **Config A** | Plain handshake | Read only |
| **Config B** | Handshake + keylog | Peek + read |
| **Payload Sizes** | Fixed (handshake) | Varying (256B-4KiB) |
| **Graph Type** | Bar chart | Line chart |
| **Port** | 19445 | 19446 |

---

## ✅ Verification Checklist

Before claiming results are valid:

- [ ] All iterations succeeded (failed_count = 0)
- [ ] CSV file has 6 rows (3 sizes × 2 configs)
- [ ] Overhead ~100% for all sizes
- [ ] Overhead increases with payload size
- [ ] Plot PNG and PDF generated
- [ ] Error bars visible on plot

---

## 📚 Additional Resources

- **Full Documentation:** [README.md](README.md) - Complete technical guide
- **Source Code:** [mb2_server_pi.c](mb2_server_pi.c), [mb2_client_remote.c](mb2_client_remote.c)
- **Related:** MB-1 (handshake overhead)
- **Next:** MB-3 (if applicable)

---

## 🎯 Ready to Start?

```bash
cd ~/Prototype_sendfd/benchmarks/micro/MB-2_read_peek
chmod +x evaluate.sh
./evaluate.sh
```

**That's all you need!** ✨

The benchmark will handle everything and save results to `results_mb2_distributed_YYYYMMDD_HHMMSS.csv`

---

**Created:** April 8, 2026  
**Status:** ✅ Ready for execution  
**Estimated Time:** 60-120 minutes (first run)
