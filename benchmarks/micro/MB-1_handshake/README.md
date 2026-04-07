# MB-1: TLS Handshake Rate Benchmark

## Overview

**Objective:** Quantify the overhead of libtlspeek's key extraction (keylog callback + HKDF derivation) compared to vanilla wolfSSL handshakes.

**Configurations:**
- **Config A:** Vanilla wolfSSL (baseline, no keylog callback)
- **Config B:** wolfSSL + keylog callback (simulating libtlspeek overhead)

**Test Sizes:** 1000, 5000, 10000 handshakes per configuration

**Metrics:** 
- Average time per handshake (µs)
- Total time (µs)
- Failed handshakes
- Percentage overhead

---

## Directory Structure

```
MB-1_handshake/
├── mb1_handshake.c       # Benchmark C code (compares A vs B)
├── run_mb1.sh            # Bash script to run benchmarks
├── plot_mb1.py           # Python script to generate graphs
├── results_mb1.csv       # Output: benchmark results (generated)
├── plot_mb1.png          # Output: grouped bar chart (generated)
├── plot_mb1.pdf          # Output: grouped bar chart PDF (generated)
└── README.md             # This file
```

---

## Quick Start

### Prerequisites

```bash
# Ensure wolfSSL is built
cd ../../.. && bash build_all.sh && cd -

# Install Python dependencies
pip3 install matplotlib numpy

# Make scripts executable
chmod +x run_mb1.sh plot_mb1.py
```

### Run Benchmark (2 steps)

**Step 1: Run measurements**
```bash
bash run_mb1.sh
```
This will:
- Compile `mb1_handshake` binary
- Run benchmarks for Config A and Config B
- Generate `results_mb1.csv`

**Step 2: Generate graph**
```bash
python3 plot_mb1.py
```
This will:
- Read `results_mb1.csv`
- Generate `plot_mb1.png` and `plot_mb1.pdf`
- Print summary statistics

---

## What Each File Does

### `mb1_handshake.c`

The main benchmark program. Measures TLS 1.3 handshake rates under two configurations.

**Configuration A (Vanilla):**
```c
/* No keylog callback */
ctx = wolfSSL_CTX_new(...);
/* Perform N handshakes and measure time */
for (i = 0; i < num_handshakes; i++) {
    /* TLS handshake */
}
```

**Configuration B (With keylog):**
```c
/* Install keylog callback (overhead we're measuring) */
wolfSSL_CTX_set_keylog_callback(ctx, keylog_cb_simulated);
ctx = wolfSSL_CTX_new(...);
/* Perform N handshakes and measure time */
for (i = 0; i < num_handshakes; i++) {
    /* TLS handshake - keylog fires during handshake */
}
```

**Usage:**
```bash
./mb1_handshake <config> <num_handshakes>
./mb1_handshake A 1000    # 1000 vanilla handshakes
./mb1_handshake B 5000    # 5000 handshakes with keylog
```

**Output:** CSV line
```
config,num_handshakes,total_time_us,avg_time_per_handshake_us,failed_count
A,1000,123456.00,123.46,0
B,1000,125891.00,125.89,0
```

### `run_mb1.sh`

Orchestrates the complete benchmark workflow.

**What it does:**
1. Compiles `mb1_handshake.c` against wolfSSL library
2. Runs benchmarks for both configs at all sizes
3. Collects results into `results_mb1.csv`
4. Prints summary statistics with overhead analysis

**Output files:**
- `results_mb1.csv` - Raw benchmark data

---

### `plot_mb1.py`

Analyzes results and generates publication-quality graphs.

**Features:**
- Reads CSV results
- Calculates mean and standard deviation
- Creates grouped bar chart comparing Config A vs B
- Shows overhead percentages
- Generates both PNG and PDF versions

**Options:**
```bash
python3 plot_mb1.py                          # Default: results_mb1.csv → plot_mb1.png
python3 plot_mb1.py --csv my_results.csv     # Custom input file
python3 plot_mb1.py --output my_plot.png     # Custom output file
python3 plot_mb1.py --no-display             # Skip interactive display
```

**Output files:**
- `plot_mb1.png` - Graph in PNG format (for presentations)
- `plot_mb1.pdf` - Graph in PDF format (for papers)

---

## Expected Results

### On Modern CPUs (x86-64)

For TLS 1.3 handshakes with AES-256-GCM:

```
N = 1000 handshakes:
  Config A (Vanilla):     ~500 µs per handshake
  Config B (Keylog):      ~520 µs per handshake
  Overhead:               ~20 µs (+4%)   ✓ Good

N = 5000 handshakes:
  Config A (Vanilla):     ~510 µs
  Config B (Keylog):      ~530 µs
  Overhead:               ~20 µs (+4%)   ✓ Good

N = 10000 handshakes:
  Config A (Vanilla):     ~515 µs
  Config B (Keylog):      ~535 µs
  Overhead:               ~20 µs (+4%)   ✓ Good
```

### On ARM (Raspberry Pi)

Overhead typically 2-5% depending on TLS cipher suite:

```
N = 1000:  Config A: 400 µs → Config B: 415 µs (+3.75%)
N = 5000:  Config A: 405 µs → Config B: 420 µs (+3.7%)
N = 10000: Config A: 410 µs → Config B: 423 µs (+3.2%)
```

### Interpretation

| Overhead | Verdict |
|----------|---------|
| < 5% | ✓✓ Excellent - negligible overhead |
| 5-10% | ✓ Good - acceptable for protocol features |
| 10-20% | ~ Fair - noticeable but manageable |
| > 20% | ⚠ Significant - may need optimization |

---

## Complete Workflow (Local Machine)

```bash
# Step 1: Navigate to benchmark directory
cd /path/to/libtlspeek/benchmarks/micro/MB-1_handshake

# Step 2: Ensure build
cd ../../..
bash build_all.sh
cd benchmarks/micro/MB-1_handshake

# Step 3: Install Python dependencies
pip3 install matplotlib numpy

# Step 4: Run benchmark (takes 2-5 minutes)
bash run_mb1.sh

# Step 5: Generate graph
python3 plot_mb1.py

# Step 6: View results
cat results_mb1.csv
open plot_mb1.png  # macOS
# or
xdg-open plot_mb1.png  # Linux
```

---

## Customization

### Change Measurement Counts

Edit `run_mb1.sh` line 57:
```bash
COUNTS=(1000 5000 10000)  # Change this
```

### Run Multiple Times (for std deviation)

To get error bars, run the benchmark multiple times and append results:
```bash
bash run_mb1.sh
bash run_mb1.sh  # Append to results_mb1.csv
bash run_mb1.sh  # Append again
python3 plot_mb1.py  # Will calculate std dev from multiple runs
```

### Parallel Runs

Run on multiple cores (if timeout tolerance allows):
```bash
taskset -c 0 bash run_mb1.sh &
taskset -c 1 bash run_mb1.sh &
wait
# Combine CSV results and plot
```

---

## Troubleshooting

### Error: "wolfSSL not found"

```bash
# Rebuild wolfSSL
cd /path/to/project
bash build_all.sh
```

### Error: "matplotlib not found"

```bash
pip3 install matplotlib numpy
```

### Error: "Compilation failed"

Check compiler output:
```bash
gcc --version  # Must be GCC 7+ or Clang 5+
```

### Graph not displaying

If running headless (no GUI):
```bash
python3 plot_mb1.py --no-display
# View PNG/PDF files directly
```

---

## Advanced: Integrate with faasd Baseline

To compare with faasd gateway (for fair evaluation):

1. **Baseline:** Deploy faasd with wolfSSL + vanilla handshakes
2. **Prototype:** Deploy faasd with wolfSSL + libtlspeek

The handshake overhead (Config B) represents how much extra CPU your libtlspeek library adds to the baseline gateway handshaking process.

---

## Files Generated

| File | Purpose | Can Delete? |
|------|---------|---|
| `mb1_handshake` | Compiled binary | Yes (rebuild with `gcc ...`) |
| `results_mb1.csv` | Raw results | No (needed for graphs) |
| `plot_mb1.png` | Graph PNG | Yes (regenerate with `plot_mb1.py`) |
| `plot_mb1.pdf` | Graph PDF | Yes (regenerate with `plot_mb1.py`) |

---

## Next Steps

After MB-1 validation:

1. ✅ **MB-1** - Handshake rate (THIS BENCHMARK)
2. ⏳ **MB-2** - `tls_read_peek()` vs `wolfSSL_read()` overhead
3. ⏳ **MB-3** - Request transfer cost
4. ⏳ **MB-4** - Keep-alive routing
5. ⏳ **MB-5** - Memory footprint per connection

---

## References

- RFC 8446: TLS 1.3
- wolfSSL Documentation: https://www.wolfssl.com/documentation
- libtlspeek Design: See `libtlspeek/README.md`
