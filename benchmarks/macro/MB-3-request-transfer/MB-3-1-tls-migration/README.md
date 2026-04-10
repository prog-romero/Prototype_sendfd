# MB-3.1: TLS State Migration vs Fresh TLS 1.3 Handshake

## Overview

**MB-3.1** compares two approaches for handling TLS connections when routing requests to workers:

| Approach | Method | How It Works |
|----------|--------|-------------|
| **PATH A: TLS Migration** | Serialize + Transfer + Restore | Gateway terminates TLS, extracts state, transfers to worker via `tlspeek_serialize()` + `tlspeek_restore()` |
| **PATH B: Fresh Handshake** | TCP Forward + New TLS handshake | Gateway forwards raw TCP, worker does fresh TLS 1.3 handshake with client |

**The Question:** Is it faster to migrate an existing TLS connection state, or for the worker to do a fresh TLS handshake?

---

## What Gets Measured

### **PATH A: TLS State Migration Time**
```
TIME = tlspeek_serialize() + sendfd_with_state() + recvfd_with_state() + tlspeek_restore()
```

**Measured on worker side** (when `tlspeek_restore()` completes):
- Gateway extracts TLS state via `wolfSSL_tls_export()`
- Sends to worker via Unix socket + SCM_RIGHTS
- Worker receives and restores via `wolfSSL_tls_import()`

### **PATH B: Fresh TLS 1.3 Handshake Time**
```
TIME = wolfSSL_accept() (complete TLS 1.3 handshake)
```

**Measured on worker side** (when `wolfSSL_accept()` completes):
- Client connects to worker
- Worker and client exchange TLS 1.3 handshake messages
- Worker has usable TLS connection

---

## Expected Results

**Hypothesis:** TLS migration should be **10-100x faster** than fresh handshake

**Why:**
- Migration: ~100 µs (CPU-only, no cryptographic handshake)
- Fresh handshake: ~1-10 ms (full TLS 1.3 handshake with crypto)

---

## Directory Structure

```
MB-3-1-tls-migration/
├── config.h                 # Constants (ports, paths, certificate locations)
├── worker_migration.c       # PATH A: restore TLS state + serve
├── worker_classic.c         # PATH B: fresh TLS handshake + serve
├── client_benchmark.c       # Main benchmark client (connects to both workers)
├── gateway_migration.c      # Gateway for PATH A (template, uses existing faasd)
├── Makefile                 # Build script
├── evaluate.sh              # Orchestrator (compile, run on local machine first)
├── plot_mb3_1.py            # Generate box plots from CSV results
├── README.md                # This file
└── results/
    ├── mb3_1_results.csv    # Raw timing data (10,000 iterations)
    └── mb3_1_boxplot.png    # Box plot visualization
```

---

## File Descriptions

### **config.h**
- Ports, paths, and constants
- `GATEWAY_MIGRATION_PORT = 8443` (PATH A)
- `GATEWAY_CLASSIC_PORT = 8444` (PATH B)
- `WORKER_MIGRATION_SOCKET = /tmp/worker_migration.sock`
- `WORKER_CLASSIC_PORT = 9001`
- `ITERATIONS_TOTAL = 10000`

### **worker_migration.c**
- Listens on Unix domain socket
- Receives FD + serialized TLS state via `recvfd_with_state()`
- Measures time to call `tlspeek_restore()`
- Serves HTTP response via restored TLS connection
- Writes timing data to CSV

**Key functions used:**
```c
tlspeek_restore(ssl, &serial);  // Restore TLS state
recvfd_with_state(sock, fd, serial);  // Receive FD + state
```

### **worker_classic.c**
- Listens on TCP port 9001
- Accepts fresh TLS connection
- Measures time to complete `wolfSSL_accept()` (TLS handshake)
- Serves HTTP response
- Writes timing data to CSV

**Key function:**
```c
int ret = wolfSSL_accept(ssl);  // Fresh TLS 1.3 handshake
```

### **client_benchmark.c**
- Runs 10,000 benchmark iterations
- For each iteration:
  - Connects to PATH A worker (performs TLS handshake with gateway, gateway transfers state)
  - Connects to PATH B worker (worker does fresh TLS handshake)
  - Records both timing measurements
  - Writes to shared CSV file

### **gateway_migration.c** (Future)
- Enhanced version of existing faasd gateway
- Uses libtlspeek for TLS termination
- Calls `tlspeek_serialize()` + `sendfd_with_state()`
- Transfers connection to worker_migration

### **Makefile**
- Compiles all C programs
- Links against wolfSSL and libtlspeek
- Creates `results/` directory

### **evaluate.sh**
- Orchestrator script
- **STEP 1:** Compile all programs
- **STEP 2:** Start worker_migration in background
- **STEP 3:** Start worker_classic in background  
- **STEP 4:** Start faasd gateway (both versions)
- **STEP 5:** Run client_benchmark (10,000 iterations)
- **STEP 6:** Collect results and generate box plots
- **STEP 7:** Print summary statistics

### **plot_mb3_1.py**
- Reads CSV results
- Generates box plots showing:
  - Median, IQR (25-75%), P1-P99 whiskers
  - Both PATH A and PATH B on same plot
  - Speedup ratio displayed
- Outputs: `mb3_1_boxplot.png` and `plot_mb3_1.pdf`

---

## CSV Output Format

**File**: `results/mb3_1_results.csv`

```csv
iteration, path, time_us, time_ns_frac
1, migration, 95, 234
1, handshake, 892, 102
2, migration, 98, 567
2, handshake, 887, 445
...
10000, migration, 97, 123
10000, handshake, 894, 678
```

**Columns:**
- `iteration` — Test iteration number (1-10000)
- `path` — "migration" or "handshake"
- `time_us` — Time in microseconds
- `time_ns_frac` — Nanosecond fractional part for precision

---

## How to Run

### **Prerequisites**
```bash
# Ensure you have:
# - wolfSSL built in ../../wolfssl/
# - libtlspeek built in ../../libtlspeek/build/
# - Certificates in ../../libtlspeek/certs/
```

### **Step 1: Test on Local Machine (100 iterations)**
```bash
cd MB-3-1-tls-migration/

# Edit config.h to use 100 iterations for quick test:
sed -i 's/#define ITERATIONS_TOTAL.*/#define ITERATIONS_TOTAL 100/' config.h

# Run full pipeline
chmod +x evaluate.sh
./evaluate.sh
```

**Expected output:**
```
[STEP 1] Compiling...
[STEP 2] Starting worker_migration...
[STEP 3] Starting worker_classic...
[STEP 4] Starting faasd gateway...
[STEP 5] Running benchmarks...
[STEP 6] Generating plots...
[STEP 7] Results Summary:
  PATH A (Migration): 95.2 µs (median)
  PATH B (Handshake): 891.4 µs (median)
  Speedup: 9.4x
```

### **Step 2: Full Run on Raspberry Pi (10,000 iterations)**
```bash
# Reset to full iterations
sed -i 's/#define ITERATIONS_TOTAL.*/#define ITERATIONS_TOTAL 10000/' config.h

# Sync to Pi
scp -r MB-3-1-tls-migration/ user@pi:/home/user/MB-3-1/

# SSH into Pi and run
ssh user@pi
cd MB-3-1/
./evaluate.sh  # Takes ~5 minutes
```

### **Step 3: View Results**
```bash
# Local machine:
# View box plots
eog results/mb3_1_boxplot.png           # Eye of GNOME image viewer
pdfviewer results/mb3_1_boxplot.pdf   # Or open PDF

# View CSV data
cat results/mb3_1_results.csv | head -20
```

---

## How evaluate.sh Works

The script orchestrates the entire benchmark:

```bash
#!/bin/bash

# STEP 0: Check prerequisites
├─ Verify certificates exist
├─ Verify libraries built
└─ Create results/ directory

# STEP 1: Compile
├─ gcc -o worker_migration worker_migration.c ...
├─ gcc -o worker_classic worker_classic.c ...
├─ gcc -o client_benchmark client_benchmark.c ...
└─ echo "[OK] All targets built"

# STEP 2: Clear old results
└─ rm -f results/mb3_1_results.csv

# STEP 3: Start worker_migration
├─ ./worker_migration &
└─ sleep 1  # Wait for socket creation

# STEP 4: Start worker_classic
├─ ./worker_classic &
└─ sleep 1  # Wait for listening

# STEP 5: Start faasd gateway (both versions)
├─ gateway_migration --tls-port 8443 --worker-socket /tmp/worker_migration.sock &
├─ gateway_classic --tcp-port 8444 --worker-port 9001 &
└─ sleep 2  # Let gateways initialize

# STEP 6: Run benchmarks
├─ ./client_benchmark
│   ├─ Warmup: 100 iterations (discarded)
│   ├─ Main: 10,000 iterations
│   └─ Results → results/mb3_1_results.csv
└─ sleep 1

# STEP 7: Generate visualization
├─ python3 plot_mb3_1.py
├─ Creates: mb3_1_boxplot.png
└─ Creates: mb3_1_boxplot.pdf

# STEP 8: Print summary statistics
├─ Median, IQR, P1, P99
├─ Speedup calculation
└─ Statistical comparison

# STEP 9: Cleanup (graceful shutdown)
├─ kill worker_migration
├─ kill worker_classic
├─ kill gateway_migration
├─ kill gateway_classic
└─ echo "[DONE]"
```

---

## Interpreting Results

### **Box Plot Explanation**

```
Time (µs)
│
│        ┌─────────────┐         ┌──────────────────┐
│        │  PATH A     │         │  PATH B          │
│        │ (Migration) │         │ (Handshake)      │
│        │             │         │                  │
│    1000│             │         ├──────────────────┤  P99
│        ├─────────────┤         │                  │
│     900│             │         ├──────────────────┤  P75 (IQR top)
│        │  ╱  (median)│         │                  │
│     100│ ────────────┐         │                  │
│        │             │         ├───────┬──────────┤  P25 (IQR bottom)
│      50│             │         │       │          │
│        ├─────────────┤         ├───────┴──────────┤  P1
│        │   PATH A    │         │   PATH B         │
│        └─────────────┘         └──────────────────┘
         (tight distribution)    (wide distribution)
                                 (includes handshake full range)
```

**What to look for:**
- **PATH A box** should be much lower (faster)
- **Minimal variance** in PATH A (serialization is consistent)
- **Higher variance** in PATH B (handshake has crypto randomness)
- **Speedup ratio** = Median(PATH B) / Median(PATH A)

**Expected values:**
- PATH A: 90-110 µs (serialize + restore)
- PATH B: 800-1000 µs (full TLS 1.3 handshake)
- Speedup: **8-10x faster** with migration

---

## Troubleshooting

### **Build errors: "tlspeek_serial.h not found"**
```bash
# Check libtlspeek is built
ls -la ../../libtlspeek/build/
# Must have: libtlspeek.a, libtlspeek.so

# Update Makefile INCLUDE path if needed
```

### **Runtime: "Cannot connect to gateway"**
```bash
# Check ports are free
lsof -i :8443
lsof -i :8444

# Kill any existing processes
pkill -f worker_migration
pkill -f worker_classic
pkill -9 evaluate.sh

# Try again
```

### **Runtime: "recvfd_with_state() fails"**
```bash
# Unix socket not created properly
# Check: ls -la /tmp/worker_migration.sock

# Restart worker_migration and check logs
./worker_migration 2>&1 | head -20
```

### **CSV file empty**
```bash
# Check timing file permissions
ls -la results/mb3_1_results.csv

# Check workers are actually running
ps aux | grep worker_

# Increase debug output
grep -n "fprintf" worker_migration.c | head -5
```

---

## Key Metrics to Report

| Metric | Expected | Unit |
|--------|----------|------|
| **Migration Median** | 95 | µs |
| **Migration P99** | 120 | µs |
| **Handshake Median** | 891 | µs |
| **Handshake P99** | 950 | µs |
| **Speedup (median)** | 9.4x | ratio |
| **Variance (Migration)** | Low | - |
| **Variance (Handshake)** | High | - |

---

## Output Files Summary

| File | Generated By | Purpose |
|------|--------------|---------|
| `results/mb3_1_results.csv` | client_benchmark.c | Raw timing data |
| `results/mb3_1_boxplot.png` | plot_mb3_1.py | Main visualization |
| `results/mb3_1_boxplot.pdf` | plot_mb3_1.py | PDF version |
| `*.o` | Makefile | Object files (cleanup with `make clean`) |

---

## Next Steps

1. ✅ **Run locally** (100 iterations) to validate setup
2. ✅ **Run on Pi** (10,000 iterations) for final results
3. ✅ **Analyze statistics** (check box plot for outliers)
4. ✅ **Interpret findings** (compare to hypothesis)
5. ✅ **Document results** for thesis/publication

---

## Questions & Notes

**Q: Why measure on server side and not client side?**

A: Client-side round-trip includes:
- Network latency
- Worker processing time
- Response transmission
- All confounds the measurement

Server-side measures just the operation we care about.

**Q: Why 10,000 iterations?**

A: Provides statistical confidence for box plot analysis:
- Captures distribution shape
- Identifies outliers
- Stable median/quantile estimates

**Q: Why both migration and handshake times in same CSV?**

A: Allows per-iteration comparison and shows correlation between operations.

---

**Status:** Ready for benchmarking ✅

**Last Updated:** April 9, 2026
