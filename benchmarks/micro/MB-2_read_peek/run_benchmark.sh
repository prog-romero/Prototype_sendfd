#!/bin/bash

################################################################################
# MB-2 BENCHMARK EXECUTION SCRIPT
# 
# Runs complete MB-2 benchmark suite
# Requires client to be compiled locally and server running on Pi
# 
# Usage: bash run_benchmark.sh <PI_IP> [test_sizes]
# Example: bash run_benchmark.sh 192.168.2.2
################################################################################

set -e

PI_IP="${1:-192.168.2.2}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="results_mb2_distributed_${TIMESTAMP}.csv"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  MB-2 BENCHMARK EXECUTION                                 ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Find client binary
CLIENT_BIN="/tmp/mb2_build/mb2_client_remote"
if [ ! -f "$CLIENT_BIN" ]; then
    echo -e "${RED}✗ Client binary not found at $CLIENT_BIN${NC}"
    echo "  Run: bash setup_and_compile.sh local"
    exit 1
fi

# Test connectivity
echo -e "${YELLOW}[1/3] Testing connectivity to Pi...${NC}"
if timeout 5 bash -c "echo > /dev/tcp/$PI_IP/19446" 2>/dev/null; then
    echo -e "${GREEN}✓ Connected to Pi${NC}"
else
    echo -e "${YELLOW}⚠ Cannot reach Pi (server might not be running yet)${NC}"
    echo "  Start server on Pi first:"
    echo "    export LD_LIBRARY_PATH=\$HOME/Prototype_sendfd/wolfssl/src/.libs:\$LD_LIBRARY_PATH"
    echo "    ./mb2_server_pi"
fi

# Run benchmarks
echo ""
echo -e "${YELLOW}[2/3] Running benchmarks...${NC}"
echo "  Payload sizes: 256B to 32KB (15 samples [test data points])"
echo "  Configurations: A (read), B (peek+read)"
echo ""

"$CLIENT_BIN" "$PI_IP" > "$RESULTS_FILE" 2>&1 || {
    echo -e "${RED}✗ Benchmark failed${NC}"
    exit 1
}

# Analyze results
echo ""
echo -e "${YELLOW}[3/3] Analyzing results...${NC}"

if [ ! -f "$RESULTS_FILE" ] || [ ! -s "$RESULTS_FILE" ]; then
    echo -e "${RED}✗ No results generated${NC}"
    exit 1
fi

# Parse and display results
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  MB-2 BENCHMARK RESULTS                                   ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

python3 << 'PYEOF'
import csv
import sys

results = {}
try:
    with open('results_mb2_distributed_'+'*'.split('/')[-1], 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            size = int(row['payload_size_bytes'])
            config = row['config']
            avg_time = float(row['avg_time_per_iteration_us'])
            stddev = float(row['stddev_us'])
            
            if size not in results:
                results[size] = {}
            results[size][config] = {'avg': avg_time, 'stddev': stddev}
except:
    pass

# Display summary
if results:
    size_names = {256: '256 B', 1024: '1 KiB', 4096: '4 KiB'}
    
    print("Payload Size │ Config A (Read)      │ Config B (Peek+Read)  │ Overhead")
    print("─────────────────────────────────────────────────────────────────────────")
    
    for size in sorted(results.keys()):
        if 'A' in results[size] and 'B' in results[size]:
            a = results[size]['A']
            b = results[size]['B']
            overhead = b['avg'] - a['avg']
            overhead_pct = (overhead / a['avg']) * 100 if a['avg'] > 0 else 0
            
            print(f" {size_names[size]:11s} │ {a['avg']:8.2f} ± {a['stddev']:6.2f} µs │ {b['avg']:8.2f} ± {b['stddev']:6.2f} µs │  {overhead_pct:+.1f}%")
PYEOF

echo ""
echo "ℹ  All benchmarks completed"
echo -e "${GREEN}✓  Results saved to: $RESULTS_FILE${NC}"
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ BENCHMARK COMPLETE                                     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Next step: python3 plot_mb2.py --csv $RESULTS_FILE --no-display"
echo ""
