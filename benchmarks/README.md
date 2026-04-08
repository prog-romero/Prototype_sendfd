# Structured Benchmark Suite for libtlspeek Evaluation

## Overview

This directory contains the complete benchmark suite for evaluating libtlspeek against the classical proxy baseline, organized into three layers:

1. **Micro-Benchmarks (MB)** - Individual primitives
2. **Macro-Benchmarks (MAC)** - Synthetic workloads  
3. **Application Benchmarks (APP)** - Real applications

---

## Directory Structure

```
benchmarks/
├── micro/                          # Micro-benchmarks
│   ├── MB-1_handshake/             ✓ Ready to use
│   │   ├── mb1_handshake.c         # Benchmark code
│   │   ├── run_mb1.sh              # Evaluation script
│   │   ├── plot_mb1.py             # Graph generation
│   │   ├── README.md               # Documentation
│   │   └── results_mb1.csv         # Results (generated)
│   │
│   ├── MB-2_peek_overhead/         ⏳ To be created
│   │   ├── mb2_peek_vs_read.c
│   │   ├── run_mb2.sh
│   │   ├── plot_mb2.py
│   │   └── README.md
│   │
│   ├── MB-3_transfer_cost/         ⏳ To be created
│   │   ├── mb3_transfer_comparison.c
│   │   ├── run_mb3.sh
│   │   └── ...
│   │
│   ├── MB-4_keepalive_routing/     ⏳ To be created
│   │   └── ...
│   │
│   └── MB-5_memory_footprint/      ⏳ To be created
│       └── ...
│
├── macro/                          # Macro-benchmarks
│   ├── MAC-1_latency_vs_payload/   ⏳ To be created
│   │   ├── mac1_server.c
│   │   ├── run_mac1.sh
│   │   └── plot_mac1.py
│   │
│   ├── MAC-2_cpu_vs_throughput/    ⏳ To be created
│   │   └── ...
│   │
│   └── MAC-3_throughput_scaling/   ⏳ To be created
│       └── ...
│
├── apps/                           # Real application benchmarks
│   ├── APP-1_bcrypt/               ⏳ To be created
│   │   ├── bcrypt_worker.c         # CPU-bound: password hashing
│   │   ├── run_app1.sh
│   │   └── plot_app1.py
│   │
│   └── APP-2_url_shortener/        ⏳ To be created
│       ├── url_shortener_worker.c  # I/O-bound: SQLite + routing
│       ├── run_app2.sh
│       └── plot_app2.py
│
└── README.md  ← YOU ARE HERE
```

---

## Getting Started

### Prerequisites

```bash
# Ensure wolfSSL and libtlspeek are built
cd ../..
bash build_all.sh
cd benchmarks

# Install Python visualization tools
pip3 install matplotlib numpy pandas

# Make scripts executable
chmod +x micro/*/run_*.sh
chmod +x macro/*/run_*.sh
chmod +x apps/*/run_*.sh
```

---

## Quick Start: Run MB-1 (Now!)

Test on your **local machine first** to verify everything works:

```bash
cd micro/MB-1_handshake

# Run benchmark (2-5 minutes)
bash run_mb1.sh

# Generate graph
python3 plot_mb1.py

# View results
cat results_mb1.csv
open plot_mb1.png  # macOS
# or
xdg-open plot_mb1.png  # Linux
```

---

## Benchmark Organization Philosophy

### Each benchmark directory contains:

```
BENCHMARK/
├── *.c files               # C code implementing the benchmark
├── run_BENCHMARK.sh        # Script to execute benchmark
├── plot_BENCHMARK.py       # Python script to generate graphs
├── README.md               # Detailed documentation
├── results_BENCHMARK.csv   # Results (generated after running)
└── plot_BENCHMARK.png      # Graph output (generated)
```

### Standardized workflow:

```bash
cd benchmarks/<category>/<BENCHMARK>

# Step 1: Run measurements
bash run_BENCHMARK.sh
# Generates: results_BENCHMARK.csv

# Step 2: Analyze and plot
python3 plot_BENCHMARK.py
# Generates: plot_BENCHMARK.png, plot_BENCHMARK.pdf

# Step 3: View summary
cat results_BENCHMARK.csv
```

---

## Micro-Benchmarks (MB-1 to MB-5)

Test individual libtlspeek primitives in isolation.

| ID | Name | What | Why | Hypothesis |
|---|---|----|-----|---|
| MB-1 | Handshake Rate | keylog callback overhead | Is key extraction cheap? | H1 |
| MB-2 | Peek Overhead | tls_read_peek() vs wolfSSL_read() | How much lighter is peek? | H1 |
| MB-3 | Transfer Cost | SCM_RIGHTS vs proxy forward | Is fd transfer cheaper? | H2 |
| MB-4 | Keep-Alive Routing | peek-first vs recv-first | When to peek vs consume? | H2 |
| MB-5 | Memory Footprint | Per-connection overhead | Is tlspeek_ctx_t lightweight? | H5 |

### Run all micro-benchmarks:

```bash
cd micro
for dir in MB-*; do
    echo "Running $dir..."
    cd "$dir"
    bash run_*.sh
    python3 plot_*.py
    cd ..
done
```

---

## Macro-Benchmarks (MAC-1 to MAC-3)

Test realistic workloads with many requests.

| ID | Name | What | Why | Hypothesis |
|---|---|----|-----|---|
| MAC-1 | Latency vs Payload | End-to-end latency at different body sizes | Does zero-copy help large requests? | H4 |
| MAC-2 | CPU vs Throughput | Gateway CPU usage vs RPS | Is gateway CPU lower? | H3 |
| MAC-3 | Throughput Scaling | Linear scaling with worker count | Gateway bottleneck? | H3 |

### Run all macro-benchmarks:

```bash
cd macro
for dir in MAC-*; do
    echo "Running $dir..."
    cd "$dir"
    bash run_*.sh
    python3 plot_*.py
    cd ..
done
```

---

## Application Benchmarks (APP-1, APP-2)

Test realistic applications.

| ID | Name | Application | Workload | Why |
|---|---|----|--------|---|
| APP-1 | bcrypt | Password hashing | CPU-bound (100ms per hash) | Amplifies gateway overhead |
| APP-2 | URL Shortener | SQLite + routing | I/O-bound + mixed routes | Tests realistic SaaS |

### Run all app benchmarks:

```bash
cd apps
for dir in APP-*; do
    echo "Running $dir..."
    cd "$dir"
    bash run_*.sh
    python3 plot_*.py
    cd ..
done
```

---

## Complete Evaluation Workflow

### Phase 1: Local Machine (Now - 30 min)

```bash
# Test one benchmark to verify setup
cd micro/MB-1_handshake
bash run_mb1.sh
python3 plot_mb1.py
# ✓ If successful, proceed to Phase 2
```

### Phase 2: Local Machine - All Benchmarks (2-3 hours)

```bash
# Run all micro-benchmarks
cd ../..
for dir in micro/*/; do
    (cd "$dir" && bash run_*.sh && python3 plot_*.py)
done

# Run all macro-benchmarks
for dir in macro/*/; do
    (cd "$dir" && bash run_*.sh && python3 plot_*.py)
done

# Run all app benchmarks
for dir in apps/*/; do
    (cd "$dir" && bash run_*.sh && python3 plot_*.py)
done
```

### Phase 3: Raspberry Pi (Official evaluation - 4-5 hours)

```bash
# SSH to Pi
ssh romero@192.168.2.2

# Repeat same benchmarks on ARM hardware
# Results will differ (ARM is slower) but relative performance unchanged
```

---

## Results Collection

After running all benchmarks, you'll have:

```
benchmarks/
├── micro/
│   ├── MB-1_handshake/
│   │   ├── results_mb1.csv
│   │   ├── plot_mb1.png
│   │   └── plot_mb1.pdf
│   ├── MB-2_peek_overhead/
│   │   ├── results_mb2.csv
│   │   ├── plot_mb2.png
│   │   └── ...
│   └── ...
├── macro/
│   └── ...
└── apps/
    └── ...
```

### Generate summary report:

```bash
python3 << 'EOF'
import os
import csv

results = {}
for root, dirs, files in os.walk('.'):
    for file in files:
        if file.startswith('results_') and file.endswith('.csv'):
            path = os.path.join(root, file)
            with open(path) as f:
                first_line = f.readline()
                print(f"{path}: {len(f.readlines())} measurements")

EOF
```

---

## Testing Strategy: Local → Pi

### Why test locally first?

1. **Fast feedback** - Minutes, not hours
2. **Easier debugging** - GUI available
3. **Catch errors early** - Fix before Pi transfer
4. **Iterate rapidly** - Try different configurations

### Testing order:

```
Local Machine (x86-64):
  MB-1 (5 min) → Verify setup works
  MB-2-5 (20 min) → Test all micro primitives
  MAC-1-3 (60 min) → Test synthetic workloads
  APP-1-2 (60 min) → Test real applications
  
  ✓ If all green, proceed to Pi
  
Raspberry Pi (ARM64):
  Repeat same benchmarks (4-5 hours)
  Collect official results
  Analyze overhead ratios
```

---

## Expected Timeline

| Phase | Duration | Machine | What |
|---|-------| ---|---|
| **MB-1 Verification** | 5 min | Local | Check build & setup |
| **All Micro-Benchmarks** | 20 min | Local | Test primitives |
| **All Macro-Benchmarks** | 60 min | Local | Test synthetic workloads |
| **All App Benchmarks** | 60 min | Local | Test real applications |
| **Local Total** | ~2.5 hours | Local | Complete dry run |
| **Pi Transfer** | 10 min | - | `rsync` code to Pi |
| **Pi Full Suite** | 4-5 hours | Pi | Official results |
| **Analysis** | 30 min | Local | Generate report |
| **GRAND TOTAL** | ~7 hours | Both | Complete evaluation |

---

## Customization

### Run only specific benchmarks:

```bash
# Just micro-benchmarks
cd micro && for d in MB-*/; do (cd "$d" && bash run_*.sh && python3 plot_*.py); done

# Just MB-1 and MB-2
cd micro/MB-1_handshake && bash run_mb1.sh && python3 plot_mb1.py
cd ../MB-2_peek_overhead && bash run_mb2.sh && python3 plot_mb2.py

# Just APP-1
cd apps/APP-1_bcrypt && bash run_app1.sh && python3 plot_app1.py
```

### Change test parameters:

Edit `run_*.sh` files in each benchmark directory to adjust:
- Number of iterations
- Payload sizes
- Request rates
- Duration

---

## Troubleshooting

### "Build failed" error

```bash
cd ../..
bash build_all.sh  # Rebuild everything
cd benchmarks
```

### "matplotlib not found"

```bash
pip3 install matplotlib numpy pandas
```

### Out of memory on Pi

Reduce benchmark size:
- Decrease handshake counts (MB-1: 1000 instead of 10000)
- Decrease request counts (MAC-1: 100 req/s instead of 5000)

### Permission denied on scripts

```bash
chmod +x micro/*/run_*.sh
chmod +x macro/*/run_*.sh
chmod +x apps/*/run_*.sh
chmod +x micro/*/plot_*.py
chmod +x macro/*/plot_*.py
chmod +x apps/*/plot_*.py
```

---

## Integration with faasd Baseline

For fair comparison described in your evaluation plan:

1. **Baseline Configuration:**
   - faasd gateway
   - wolfSSL + vanilla handshakes (Config A from MB-1)
   - Classical proxy path (forward entire request)

2. **Prototype Configuration:**
   - faasd gateway logic (reused)
   - wolfSSL + libtlspeek (Config B from MB-1)
   - SCM_RIGHTS fd transfer (no forwarding)

The overhead measured in these benchmarks represents the architectural advantage of libtlspeek over classical proxying.

---

## Next: Create Remaining Benchmarks

Template for MB-2, MB-3, etc.:

```bash
mkdir micro/MB-2_peek_overhead
cp micro/MB-1_handshake/run_*.sh micro/MB-2_peek_overhead/
# Edit run_mb2.sh to compile mb2_peek_vs_read.c instead
# Follow same pattern
```

---

## Questions?

- **MB-1 not working?** → See `micro/MB-1_handshake/README.md`
- **Can't build?** → Run `../../build_all.sh` first
- **Graph not displaying?** → Use `--no-display` flag
- **Want custom parameters?** → Edit respective `run_*.sh` file

---

## Ready to start?

```bash
cd micro/MB-1_handshake
bash run_mb1.sh
```

Let's go! 🚀
