# MB-1 Setup Checklist & Quick Start

## What I've Created For You

✅ **Complete MB-1 Benchmark Suite** (Highly Structured):

```
benchmarks/micro/MB-1_handshake/
├── mb1_handshake.c       # C code: Compares vanilla wolfSSL vs with keylog callback
├── run_mb1.sh            # Bash script: Executes benchmark + collects results
├── plot_mb1.py           # Python: Generates publication-quality graphs  
├── README.md             # Full documentation
└── benchmarks/README.md  # Overall structure guide
```

---

## What Each Component Does

### `mb1_handshake.c` - The Benchmark Code

Measures TLS 1.3 handshake performance in two configurations:

**Configuration A (Vanilla):**
- Standard wolfSSL without keylog callback
- This is your BASELINE
- Represents "normal" TLS overhead

**Configuration B (With keylog):**
- wolfSSL WITH keylog callback 
- Simulates libtlspeek's key extraction overhead
- Shows the COST of your library feature

**How it measures:**
1. Takes N handshakes (1000, 5000, or 10000)
2. Measures total time and average per handshake
3. Outputs results in CSV format

---

### `run_mb1.sh` - The Evaluation Script

Orchestrates everything:

```bash
bash run_mb1.sh
```

**Step-by-step:**
1. ✓ Checks wolfSSL library exists
2. ✓ Compiles `mb1_handshake.c`
3. ✓ Runs Config A benchmarks (1000, 5000, 10000 handshakes)
4. ✓ Runs Config B benchmarks (1000, 5000, 10000 handshakes)
5. ✓ Saves results to `results_mb1.csv`
6. ✓ Prints summary with overhead analysis

**Examples of what you'll see:**
```
[1/3] Compiling benchmark...
[✓] Compiled: ./mb1_handshake

[2/3] Running benchmarks...
Configuration A:     ← Vanilla wolfSSL
  Testing 1000 handshakes...
  Testing 5000 handshakes...
  Testing 10000 handshakes...

Configuration B:     ← With keylog callback
  Testing 1000 handshakes...
  Testing 5000 handshakes...
  Testing 10000 handshakes...

[3/3] Analyzing results...
Results Summary:
config num_handshakes total_time_us avg_time_per_handshake_us failed_count
A      1000           456789       456.79                     0
A      5000           2300000      460.00                     0
A      10000          4600000      460.00                     0
B      1000           480000       480.00                     0
B      5000           2400000      480.00                     0
B      10000          4800000      480.00                     0

N=1000:
  Config A: 456.79 µs/handshake
  Config B: 480.00 µs/handshake
  Overhead: 23.21 µs/handshake (+5.1%)
```

---

### `plot_mb1.py` - The Graph Generator

Converts CSV data into publication-quality graphs:

```bash
python3 plot_mb1.py
```

**Generates:**
- `plot_mb1.png` - High-resolution graph (for presentations)
- `plot_mb1.pdf` - Vector graph (for research papers)

**Graph shows:**
- Grouped bars: Config A vs Config B
- Error bars: Standard deviation (if multiple runs)
- Values labeled on each bar
- Overhead percentages in summary table

**Example output printed to console:**
```
======================================================================
MB-1: TLS Handshake Rate Benchmark - Summary
======================================================================

N = 1000 handshakes:
  Config A (Vanilla):     456.79 µs ± 2.45 µs
  Config B (Keylog):      480.00 µs ± 3.12 µs
  Overhead:               23.21 µs (+5.1%)
  ✓ MINIMAL overhead (<5%) - excellent!

N = 5000 handshakes:
  Config A (Vanilla):     460.00 µs ± 1.89 µs
  Config B (Keylog):      480.00 µs ± 2.45 µs
  Overhead:               20.00 µs (+4.3%)
  ✓ MINIMAL overhead (<5%) - excellent!

N = 10000 handshakes:
  Config A (Vanilla):     460.00 µs ± 1.56 µs
  Config B (Keylog):      480.00 µs ± 2.01 µs
  Overhead:               20.00 µs (+4.3%)
  ✓ MINIMAL overhead (<5%) - excellent!
```

---

## How to Use - Complete Workflow

### Step 0: Prerequisites (One Time)

```bash
# Navigate to benchmark directory
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/MB-1_handshake

# Ensure wolfSSL is built  
cd ../../..
bash build_all.sh  # Takes 5-10 minutes
cd benchmarks/micro/MB-1_handshake

# Install Python graphing tools
pip3 install matplotlib numpy

# Make scripts executable
chmod +x run_mb1.sh plot_mb1.py
```

### Step 1: Run Benchmark (Local Machine - x86-64)

```bash
cd /path/to/benchmarks/micro/MB-1_handshake

# Execute benchmark (takes 2-5 minutes)
bash run_mb1.sh

# Output: results_mb1.csv
```

### Step 2: Generate Graph

```bash
# Create graph from CSV results
python3 plot_mb1.py

# Output:
# - plot_mb1.png (view with: open plot_mb1.png)
# - plot_mb1.pdf (for paper)
```

### Step 3: Analyze Results

```bash
# View raw data
cat results_mb1.csv

# Example output:
config,num_handshakes,total_time_us,avg_time_per_handshake_us,failed_count
A,1000,456789.00,456.79,0
A,5000,2300000.00,460.00,0
A,10000,4600000.00,460.00,0
B,1000,480000.00,480.00,0
B,5000,2400000.00,480.00,0
B,10000,4800000.00,480.00,0
```

---

## Expected Results

### What You Should See

**Good Results (< 5% overhead):**
```
Config A: 456 µs per handshake
Config B: 480 µs per handshake
Overhead: 24 µs (+5.3%)  ← This is GOOD!
```
✓ Your keylog callback is lightweight

**Acceptable (5-10%):**
```
Overhead: 46 µs (+10%)  ← Still reasonable
```
~ Some overhead, but manageable

**Concerning (> 10%):**
```
Overhead: 60 µs (+15%)  ← Too much
```
⚠ May need optimization

### Why This Matters

- **< 5%** = Your library is nearly free (excellent innovation!)
- **5-10%** = Worth the cost for routing capability
- **> 10%** = Consider optimizing key derivation

---

## Quick Start (TL;DR)

```bash
# 1. Navigate
cd ~/Prototype_sendfd/benchmarks/micro/MB-1_handshake

# 2. Run (should work immediately - checks everything)
bash run_mb1.sh

# 3. Plot
python3 plot_mb1.py

# 4. Done!
# - View results in: results_mb1.csv
# - View graph: plot_mb1.png or plot_mb1.pdf
```

**Total time: 5-10 minutes**

---

## What Happens If It Fails

### "wolfSSL not found"
```bash
cd ../../..
bash build_all.sh
```

### "matplotlib not found"
```bash
pip3 install matplotlib numpy
```

### "Permission denied"
```bash
chmod +x run_mb1.sh plot_mb1.py
```

### "Compilation error"
Check that your C compiler version matches:
```bash
gcc --version  # Should be 7.0+ or Clang 5.0+
```

---

## File Locations

| File | Location | Purpose |
|------|----------|---------|
| **Source Code** | `mb1_handshake.c` | Benchmark logic |
| **Run Script** | `run_mb1.sh` | Automate everything |
| **Plot Script** | `plot_mb1.py` | Generate graph |
| **Results** | `results_mb1.csv` | Raw data (generated) |
| **Graph** | `plot_mb1.png` | Visual output (generated) |
| **Documentation** | `README.md` | Full details |

---

## Integration with Your Evaluation Plan

This MB-1 benchmark validates **Hypothesis H1** from your eval plan:

**H1:** "Peek overhead is negligible. tls_read_peek() adds minimal overhead over wolfSSL_read() for a typical HTTP header because stateless AEAD decryption is a pure mathematical operation with no session state side effects."

**What MB-1 measures:**
- keylog callback overhead (part of H1)
- Key extraction cost (HKDF work)
- Foundation for later benchmarks (MB-2-5)

**Next:** MB-2 will measure the actual `tls_read_peek()` function overhead directly.

---

## File Structure After Execution

```
benchmarks/micro/MB-1_handshake/
├── mb1_handshake.c          # Original source
├── mb1_handshake            # Compiled binary (generated)
├── run_mb1.sh               # Benchmark executor
├── plot_mb1.py              # Graph generator
├── results_mb1.csv          # ← Results go here
├── plot_mb1.png             # ← Graph PNG
├── plot_mb1.pdf             # ← Graph PDF  
└── README.md                # Details
```

---

## Success Criteria

✅ You succeeded when you see:

1. ✓ `results_mb1.csv` created with 6 data rows
2. ✓ `plot_mb1.png` and `plot_mb1.pdf` generated
3. ✓ Overhead is < 10% for all test sizes
4. ✓ Consistent overhead across 1K, 5K, 10K handshakes

---

## Next Steps After MB-1

### Local Machine - Continue Testing
1. ✅ MB-1 complete
2. ⏳ Create MB-2 (peek overhead)
3. ⏳ Create MB-3 (transfer cost)
4. ⏳ Create MB-4 (keep-alive)
5. ⏳ Create MB-5 (memory)

### After All Local Tests Pass
Transfer to Raspberry Pi and repeat

---

## Commands Summary

```bash
# Navigate
cd ~/Prototype_sendfd/benchmarks/micro/MB-1_handshake

# Build (one time)
cd ../../.. && bash build_all.sh && cd benchmarks/micro/MB-1_handshake

# Install Python deps (one time)
pip3 install matplotlib numpy

# Make executable (one time)
chmod +x run_mb1.sh plot_mb1.py

# RUN THE BENCHMARK
bash run_mb1.sh

# GENERATE GRAPH
python3 plot_mb1.py

# VIEW RESULTS
cat results_mb1.csv
open plot_mb1.png
```

---

## I'm Ready! Let's Go!

```bash
cd ~/Prototype_sendfd/benchmarks/micro/MB-1_handshake
bash run_mb1.sh
```

**Report back the results!** 🚀

---

## Questions?

- **Why two configs (A vs B)?** → To measure keylog overhead in isolation
- **Why three sizes (1K, 5K, 10K)?** → Show consistency across different workloads
- **Why generate PNG and PDF?** → PNG for presentations, PDF for research papers
- **Where's the faasd integration?** → That goes in MB-3 (transfer cost benchmark)

More details: See `benchmarks/README.md` and `micro/MB-1_handshake/README.md`

---
