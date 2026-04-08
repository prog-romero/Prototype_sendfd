# MB-2: TLS read_peek() vs wolfSSL_read() Overhead Analysis

## 🎯 Benchmark Objective

**Goal:** Quantify the performance overhead of inspecting (peeking at) encrypted TLS records before decryption, compared to just reading the data normally.

**Why It Matters:**
- libtlspeek needs to inspect decrypted plaintext **before** returning it to the application
- This requires **decrypting twice**: (1) peek, (2) read
- MB-2 measures this double-decryption cost across different payload sizes

---

## 📊 Technical Logic Explained

### The Problem libtlspeek Solves

```
Normal TLS Flow:
┌─────────────────────────────┐
│ Encrypted Data (network)   │
└────────────┬────────────────┘
             │ wolfSSL_read()
             ├─ Decrypt
             └─> Application gets plaintext

libtlspeek Flow (what we need to measure):
┌─────────────────────────────┐
│ Encrypted Data (network)   │
└────────────┬────────────────┘
             │ tls_read_peek()
             ├─ Decrypt (FIRST TIME)
             ├─ Inspect plaintext
             ├─ Extract secrets/keys
             └─> Internal buffer (not returned)
             │ wolfSSL_read()
             ├─ Decrypt (SECOND TIME)  ← OVERHEAD!
             └─> Application gets plaintext
```

### What MB-2 Measures

**Config A: Baseline (wolfSSL_read only)**
```
Time_A = Time to decrypt and read data once
```

**Config B: With peek overhead (peek + read)**
```
Time_B = Time to decrypt (peek) + Time to decrypt (read)
       = 2 × Time to decrypt + small overhead
```

**Overhead Calculation:**
```
Overhead = Time_B - Time_A
         ≈ Time to decrypt once (the peek operation)
```

### Why Overhead Varies with Payload Size

AEAD (Authenticated Encryption with Associated Data) decryption cost scales with **payload size**:

- **256 B payload:** Fast decryption → Small overhead
- **1 KiB payload:** Moderate decryption → Medium overhead  
- **4 KiB payload:** Slower decryption → Larger overhead

**Expected relationship:** Overhead ∝ Payload Size (roughly linear)

---

## 📋 Test Design

### Test Matrix

| Payload Size | Config A | Config B | Measure |
|-------------|----------|---------|---------|
| **256 B**   | wolfSSL_read() | Peek + Read | Overhead_256B |
| **1 KiB**   | wolfSSL_read() | Peek + Read | Overhead_1KiB |
| **4 KiB**   | wolfSSL_read() | Peek + Read | Overhead_4KiB |

### Execution Strategy

For each payload size:
1. **Config A Run:** Read encrypted data N times, measure total time
2. **Config B Run:** Peek then read encrypted data N times, measure total time
3. **Calculate Overhead:** `(Time_B - Time_A) / Time_A × 100%`

**N = 1000 iterations per size** (statistical confidence)

### Expected Results

```
Size      │ Time_A (A alone) │ Time_B+A (Peek+Read) │ Difference
──────────┼──────────────────┼─────────────────────┼────────────
256 B     │ 100 µs           │ 200 µs              │ 100 µs
1 KiB     │ 352 µs           │ 678 µs              │ 326 µs
4 KiB     │ 1232 µs          │ 2456 µs             │ 1224 µs

Overhead grows linearly with payload size
```

---

## 🚀 Quick Start - All Commands

### One-Command Full Execution

```bash
cd ~/Prototype_sendfd/benchmarks/micro/MB-2_read_peek
chmod +x evaluate.sh
./evaluate.sh
```

**Time:** 60-120 minutes (first run with builds)

### Manual Step-by-Step

**Step 1: Sync to Pi**
```bash
bash full_sync_to_pi.sh romero 192.168.2.2
```

**Step 2: Compile Server (Pi terminal)**
```bash
ssh romero@192.168.2.2
cd ~/Prototype_sendfd/benchmarks/micro/MB-2_read_peek
bash setup_and_compile.sh pi
```

**Step 3: Compile Client (Local)**
```bash
bash setup_and_compile.sh local
```

**Step 4: Start Server (Pi terminal)**
```bash
export LD_LIBRARY_PATH=$HOME/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH
./mb2_server_pi
# Output: [SERVER] Listening on 0.0.0.0:19446
```

**Step 5: Run Benchmarks (Local, new terminal)**
```bash
bash run_benchmark.sh 192.168.2.2
# Payload sizes: 256 B, 1 KiB, 4 KiB
```

**Step 6: Generate Plot**
```bash
python3 plot_mb2.py --csv results_mb2_distributed_*.csv --no-display
# Creates: plot_mb2.png and plot_mb2.pdf
```

---

## 📁 File Structure

```
MB-2_read_peek/
├── 📜 README.md                              # This file
├── 🚀 evaluate.sh                            # One-command execution
├── 📦 Core Scripts
│   ├── full_sync_to_pi.sh                   # Project syncer
│   ├── setup_and_compile.sh                 # Build script
│   └── run_benchmark.sh                     # Benchmark executor
├── 📝 Source Code
│   ├── mb2_server_pi.c                      # Server (Pi)
│   └── mb2_client_remote.c                  # Client (local)
├── 📊 Analysis
│   ├── plot_mb2.py                          # Graph generator
│   ├── plot_mb2.png                         # Generated chart
│   └── plot_mb2.pdf                         # Generated PDF
└── 📁 Results (Generated)
    └── results_mb2_distributed_*.csv        # Raw results
```

---

## 🔧 How the Benchmark Works

### Server Side (mb2_server_pi.c)

```c
// Create TLS connection with client
// Generate test payloads: 256B, 1KiB, 4KiB
// Send data through TLS stream
// Repeat N times for each size

for (size_t iter = 0; iter < iterations; iter++) {
    for (size_t payload_idx = 0; payload_idx < N_SIZES; payload_idx++) {
        // Send test_data[payload_size] through SSL connection
        wolfSSL_write(ssl, test_data[payload_idx], sizes[payload_idx]);
    }
}
```

### Client Side (mb2_client_remote.c)

```c
// Config A: Normal read
start_timer();
for (size_t iter = 0; iter < iterations; iter++) {
    for (size_t payload_idx = 0; payload_idx < N_SIZES; payload_idx++) {
        // Just read the data
        wolfSSL_read(ssl, buffer, sizes[payload_idx]);
    }
}
time_config_a = stop_timer();

// Config B: Peek then read
start_timer();
for (size_t iter = 0; iter < iterations; iter++) {
    for (size_t payload_idx = 0; payload_idx < N_SIZES; payload_idx++) {
        // Peek at decrypted data (decrypt once internally)
        tls_read_peek(ssl, buffer);
        
        // Then read normally (decrypt again)
        wolfSSL_read(ssl, buffer, sizes[payload_idx]);
    }
}
time_config_b = stop_timer();

// Calculate overhead
overhead_us = time_config_b - time_config_a;
```

---

## 📈 Interpreting Results

### CSV Output Format

```csv
payload_size_bytes,config,iterations,total_time_us,avg_time_per_iteration_us,stddev_us,failed_count
256,A,1000,102000.00,102.00,5.23,0
256,B,1000,205000.00,205.00,8.15,0
1024,A,1000,358200.00,358.20,12.45,0
1024,B,1000,681500.00,681.50,19.83,0
4096,A,1000,1232000.00,1232.00,45.20,0
4096,B,1000,2456000.00,2456.00,67.50,0
```

### Calculating Overhead

```python
overhead_256b = (205.00 - 102.00) / 102.00 * 100 = 101.0%  (roughly 100% = one extra decrypt)
overhead_1kb  = (681.50 - 358.20) / 358.20 * 100 = 90.3%
overhead_4kb  = (2456.00 - 1232.00) / 1232.00 * 100 = 99.4%
```

### What to Expect

**Expected Pattern:**
```
Overhead ≈ 100% for all sizes (roughly)
Reason: peek + read ≈ 2× single read
```

**What Would Be Concerning:**
```
Overhead > 150%:   Inefficient implementation
Overhead < 50%:    Caching/optimization issue (peek might be skipped)
Overhead = 100%:   Perfect - peek costs exactly one decryption
```

---

## 📊 Graph Interpretation

The generated `plot_mb2.png` shows:

```
Overhead (µs)
    │
    │     ●─────●
    │    /       \  (Should be roughly linear)
    │   /         ●
    │  /
    └──────────────→ Payload Size (bytes)
   256B   1KiB   4KiB
```

**Key Features:**
- **X-axis:** Payload size (256 B, 1 KiB, 4 KiB)
- **Y-axis:** Overhead in microseconds
- **Points:** Average overhead for each size
- **Error bars:** Standard deviation across iterations
- **Trend:** Should be roughly linear (overhead ∝ size)

---

## 💡 Key Insights from MB-2

### What This Benchmark Reveals

1. **Cost of Double Decryption:** How much extra time peek adds
2. **Scalability:** Does overhead grow linearly or superlinearly with size?
3. **Feasibility:** Is the overhead acceptable for production use?
4. **Optimization Potential:** Where to optimize for larger payloads

### Why Overlay Two Decryptions?

✅ **libtlspeek architecture requires it:**
- First decryption (peek): Extract & validate keys
- Second decryption (read): Return plaintext to app

❌ **Can't be avoided without:**
- Changing TLS spec (not practical)
- Caching decrypted state (complex, security issues)
- Modifying wolfSSL internals (maintenance burden)

### Production Implications

```
If overhead is <10%:   Excellent fit for all use cases
If overhead is 50-100%: Acceptable for non-latency-critical apps
If overhead is >200%:   May need optimization for production
```

---

## ⚠️ Troubleshooting

### "tls_read_peek not found"
```bash
# Ensure libtlspeek headers are in path
ls ~/Prototype_sendfd/libtlspeek/lib/*.h

# May need to modify compile flags
grep -r "include.*tls_read_peek" mb2_client_remote.c
```

### "Server won't send large payloads"
```bash
# Check TLS record size limits
# wolfSSL defaults to 16KB records
# Our test uses 4KiB, should be fine
```

### "Overhead seems wrong (>500%)"
```bash
# Network latency might dominate
# Try on same machine or local network
# Check server and client both running on 192.168.2.0/24
```

---

## 📝 Expected Results Template

```
╔════════════════════════════════════════════════════════════╗
║  MB-2 PAYLOAD SIZE OVERHEAD ANALYSIS                       ║
╚════════════════════════════════════════════════════════════╝

Payload    │ Config A (Read)  │ Config B (Peek+Read)  │ Overhead
────────────────────────────────────────────────────────────────
256 B      │ 102 ± 5 µs      │ 205 ± 8 µs           │ +101%
1 KiB      │ 358 ± 12 µs     │ 681 ± 20 µs          │ +90%
4 KiB      │ 1232 ± 45 µs    │ 2456 ± 68 µs         │ +99%

Average overhead: ~97% (Peek adds roughly one full decryption)

Conclusion: ✓ Overhead scales linearly with payload size
            ✓ Peak cost is approximately 100% (expected)
            ✓ Implementation is efficient
```

---

## 🎯 Next Steps

After MB-2 completes:
1. Review results for payload size scaling
2. Archive plot for presentation
3. Move to MB-3 (key extraction overhead)

---

**Status:** ✅ Ready for execution
**Created:** April 8, 2026
