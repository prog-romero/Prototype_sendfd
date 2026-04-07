#!/bin/bash

#
# MB-1: Run TLS Handshake Rate Benchmark
#
# Tests:
#   - Configuration A: Vanilla wolfSSL (baseline)
#   - Configuration B: With keylog callback (libtlspeek overhead)
#
# Tests multiple handshake counts: 1000, 5000, 10000
#
# Output: results_mb1.csv with columns:
#   config,num_handshakes,total_time_us,avg_time_per_handshake_us,failed_count
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WOLFSSL_PATH="$PROJECT_ROOT/wolfssl"
LIBTLSPEEK_PATH="$PROJECT_ROOT/libtlspeek"

RESULTS_FILE="$SCRIPT_DIR/results_mb1.csv"
BINARY="$SCRIPT_DIR/mb1_handshake"

echo "════════════════════════════════════════════════════════════"
echo "MB-1: TLS Handshake Rate Benchmark"
echo "════════════════════════════════════════════════════════════"
echo ""

# ─────────────────────────────────────────────────────────────────
# Step 1: Compile benchmark
# ─────────────────────────────────────────────────────────────────

echo "[1/3] Compiling benchmark..."

# Check wolfSSL library (support both static and dynamic)
WOLFSSL_LIB=""
if [ -f "$WOLFSSL_PATH/src/.libs/libwolfssl.a" ]; then
    WOLFSSL_LIB="$WOLFSSL_PATH/src/.libs/libwolfssl.a"
elif [ -f "$WOLFSSL_PATH/src/.libs/libwolfssl.so" ]; then
    WOLFSSL_LIB="$WOLFSSL_PATH/src/.libs/libwolfssl.so"
else
    echo "[ERROR] wolfSSL library not found at $WOLFSSL_PATH/src/.libs/"
    echo "        Available files:"
    ls -lah "$WOLFSSL_PATH/src/.libs/" | grep -E "libwolfssl\.(a|so)" || echo "        (none found)"
    echo ""
    echo "        Run: cd $PROJECT_ROOT && bash build_all.sh"
    exit 1
fi

# Set library path for runtime
export LD_LIBRARY_PATH="$WOLFSSL_PATH/src/.libs:$LD_LIBRARY_PATH"

# Compile with wolfSSL
echo "  Compiling..."
gcc -O2 -Wall -Wextra \
    mb1_handshake.c \
    -o "$BINARY" \
    -I"$WOLFSSL_PATH" \
    -I"$LIBTLSPEEK_PATH/lib" \
    -I"$LIBTLSPEEK_PATH/common" \
    -L"$WOLFSSL_PATH/src/.libs" \
    -lwolfssl \
    -lm \
    -pthread \
    -Wl,-rpath,"$WOLFSSL_PATH/src/.libs" \
    2>&1 | grep -E "error:|^mb1_handshake" || true

# Check if compilation succeeded
if [ ! -f "$BINARY" ]; then
    echo "[ERROR] Compilation failed - binary not created"
    exit 1
fi

BINARY_SIZE=$(stat -c%s "$BINARY" 2>/dev/null || stat -f%z "$BINARY")
echo "[✓] Compiled: $BINARY ($BINARY_SIZE bytes)"
echo ""

# ─────────────────────────────────────────────────────────────────
# Step 2: Run benchmarks
# ─────────────────────────────────────────────────────────────────

echo "[2/3] Running benchmarks..."
echo ""

# CSV header (new format includes stddev)
echo "config,num_handshakes,total_time_us,avg_time_per_handshake_us,stddev_time_us,failed_count" > "$RESULTS_FILE"

# Test configurations and handshake counts
CONFIGS=("A" "B")
COUNTS=(5000 6000 8000 10000)

for config in "${CONFIGS[@]}"; do
    echo "Configuration $config:"
    
    for count in "${COUNTS[@]}"; do
        echo "  Testing $count handshakes..."
        
        # Run benchmark and append to CSV
        LD_LIBRARY_PATH="$WOLFSSL_PATH/src/.libs:$LD_LIBRARY_PATH" \
        "$BINARY" "$config" "$count" >> "$RESULTS_FILE"
        
        sleep 1  # Brief pause between tests
    done
    
    echo ""
done

echo "[✓] All benchmarks complete!"
echo ""

# ─────────────────────────────────────────────────────────────────
# Step 3: Analyze results
# ─────────────────────────────────────────────────────────────────

echo "[3/3] Analyzing results..."
echo ""

echo "Results Summary:"
echo "─────────────────────────────────────────────────────────────"
cat "$RESULTS_FILE" | column -t -s ','
echo ""

# Calculate overhead
echo "Overhead Analysis (Config B vs Config A):"
echo "─────────────────────────────────────────────────────────────"

python3 << PYTHON_ANALYSIS
import csv
import sys

# Read CSV from file
results_file = "$RESULTS_FILE"
rows = []
try:
    with open(results_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
except Exception as e:
    print(f"[ERROR] Failed to read {results_file}: {e}", file=sys.stderr)
    sys.exit(1)

# Group by handshake count
by_count = {}
for row in rows:
    count = int(row['num_handshakes'])
    if count not in by_count:
        by_count[count] = {}
    by_count[count][row['config']] = {
        'avg_time': float(row['avg_time_per_handshake_us']),
        'stddev': float(row['stddev_time_us']),
        'failed': int(row['failed_count'])
    }

# Calculate overhead
for count in sorted(by_count.keys()):
    if 'A' in by_count[count] and 'B' in by_count[count]:
        a_data = by_count[count]['A']
        b_data = by_count[count]['B']
        a = a_data['avg_time']
        b = b_data['avg_time']
        a_std = a_data['stddev']
        b_std = b_data['stddev']
        overhead_pct = ((b - a) / a) * 100
        overhead_us = b - a
        print(f"N={count:,}:")
        print(f"  Config A: {a:8.2f} ± {a_std:6.2f} µs/handshake")
        print(f"  Config B: {b:8.2f} ± {b_std:6.2f} µs/handshake")
        print(f"  Overhead: {overhead_us:8.2f} µs/handshake ({overhead_pct:+.1f}%)")
        print()
PYTHON_ANALYSIS

echo ""
echo "[✓] Results saved to: $RESULTS_FILE"
echo ""
echo "Next step: Generate graph"
echo "  Run: python3 plot_mb1.py"
echo ""
