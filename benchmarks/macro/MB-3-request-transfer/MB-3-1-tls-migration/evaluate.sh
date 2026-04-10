#!/bin/bash
set -e

# Set up library paths for runtime linking
export LD_LIBRARY_PATH="../../../../libtlspeek/build:../../../../wolfssl/src/.libs:${LD_LIBRARY_PATH}"

# MB-3.1 Benchmark Evaluation Script
# 
# Orchestrates complete evaluation:
# Phase 1: Test PATH A (TLS Migration) - gateway + worker_migration_complete + client
# Phase 2: Test PATH B (Fresh Handshake) - worker_classic + client  
# Phase 3: Merge results and generate comparison plots

echo "═══════════════════════════════════════════════════════════════"
echo "  MB-3.1: TLS Migration vs Fresh Handshake Benchmark"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# STEP 1: Verify Prerequisites
# ─────────────────────────────────────────────────────────────────────────── #

echo "[INIT] Checking prerequisites..."

# Check for certificates
if [ ! -f "../../../../libtlspeek/certs/server.crt" ]; then
    echo "ERROR: Server certificate not found at ../../../../libtlspeek/certs/server.crt"
    exit 1
fi

if [ ! -f "../../../../libtlspeek/certs/server.key" ]; then
    echo "ERROR: Server key not found at ../../../../libtlspeek/certs/server.key"
    exit 1
fi

echo "✓ Certificates found"

# Check for libraries
if [ ! -f "../../../../libtlspeek/build/libtlspeek.a" ] && [ ! -f "../../../../libtlspeek/build/libtlspeek.so" ]; then
    echo "ERROR: libtlspeek not built at ../../../../libtlspeek/build/"
    echo "Please run: cd ../../../../libtlspeek && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

echo "✓ libtlspeek found"

if [ ! -f "../../../../wolfssl/src/.libs/libwolfssl.a" ] && [ ! -f "../../../../wolfssl/src/.libs/libwolfssl.so" ]; then
    echo "ERROR: wolfSSL not built at ../../../../wolfssl/src/.libs/"
    exit 1
fi

echo "✓ wolfSSL found"
echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# STEP 2: Compile All Programs
# ─────────────────────────────────────────────────────────────────────────── #

echo "[BUILD] Compiling benchmark programs..."
make clean >/dev/null 2>&1 || true
make all

echo "✓ All programs built successfully"
echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# STEP 3: Clean Old Results
# ─────────────────────────────────────────────────────────────────────────── #

echo "[CLEANUP] Removing old results..."
rm -f results/mb3_1_*.csv
mkdir -p results

echo "✓ Results directory clean"

# ─────────────────────────────────────────────────────────────────────────── #
# CLEANUP: Kill lingering processes and wait for ports to be free
# ─────────────────────────────────────────────────────────────────────────── #

echo "[CLEANUP] Killing lingering processes..."
pkill -9 -f "gateway" 2>/dev/null || true
pkill -9 -f "worker_migration_complete" 2>/dev/null || true
pkill -9 -f "worker_classic" 2>/dev/null || true
pkill -9 -f "client_benchmark" 2>/dev/null || true

# Remove stale socket file
rm -f /tmp/worker_migration.sock

# Wait for ports to be released from TIME_WAIT state
echo "[CLEANUP] Waiting for ports 8443 and 9001 to be released..."
for i in {1..30}; do
    if ! nc -z 127.0.0.1 8443 2>/dev/null && ! nc -z 127.0.0.1 9001 2>/dev/null; then
        echo "✓ Ports are free"
        break
    fi
    sleep 1
done

sleep 2
echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# PHASE 1: TEST PATH A (TLS Migration)
# ─────────────────────────────────────────────────────────────────────────── #

echo "═══════════════════════════════════════════════════════════════"
echo "  PHASE 1: Testing PATH A (TLS Migration)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo "[PHASE-1] Starting worker_migration_complete..."
./worker_migration_complete &
WORKER_MIGRATION_PID=$!
sleep 3

echo "[PHASE-1] Starting gateway (TLS termination)..."
./gateway &
GATEWAY_PID=$!
sleep 3

echo "[PHASE-1] Verifying gateway is listening..."
for i in {1..10}; do
    if nc -z 127.0.0.1 8443 2>/dev/null; then
        break
    fi
    echo "[PHASE-1] Waiting for gateway ($i/10)..."
    sleep 1
done

if ! nc -z 127.0.0.1 8443 2>/dev/null; then
    echo "ERROR: Gateway not listening on port 8443"
    kill $GATEWAY_PID $WORKER_MIGRATION_PID 2>/dev/null || true
    exit 1
fi

echo "✓ Gateway and worker_migration ready"
echo ""

echo "[PHASE-1] Running benchmark client (PATH A, TLS Migration)..."
./client_benchmark --path A

echo "✓ PATH A benchmark complete"

echo "[PHASE-1] Stopping gateway and worker..."
kill $GATEWAY_PID $WORKER_MIGRATION_PID 2>/dev/null || true
sleep 1

echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# PHASE 2: TEST PATH B (Fresh Handshake)
# ─────────────────────────────────────────────────────────────────────────── #

echo "═══════════════════════════════════════════════════════════════"
echo "  PHASE 2: Testing PATH B (Fresh TLS 1.3 Handshake)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Cleanup before starting PHASE-2
echo "[PHASE-2] Cleaning up from previous phase..."
pkill -9 -f "worker_classic" 2>/dev/null || true
pkill -9 -f "client_benchmark" 2>/dev/null || true

# Wait for port 9001 to be released
echo "[PHASE-2] Waiting for port 9001 to be released..."
for i in {1..20}; do
    if ! nc -z 127.0.0.1 9001 2>/dev/null; then
        echo "✓ Port 9001 is free"
        break
    fi
    sleep 1
done

sleep 2

echo "[PHASE-2] Starting worker_classic..."
./worker_classic &
WORKER_CLASSIC_PID=$!
sleep 2

echo "[PHASE-2] Verifying worker_classic is listening..."
for i in {1..10}; do
    if nc -z 127.0.0.1 9001 2>/dev/null; then
        break
    fi
    echo "[PHASE-2] Waiting for worker_classic ($i/10)..."
    sleep 1
done

if ! nc -z 127.0.0.1 9001 2>/dev/null; then
    echo "ERROR: worker_classic not listening on port 9001"
    kill $WORKER_CLASSIC_PID 2>/dev/null || true
    exit 1
fi

echo "✓ worker_classic ready"
echo ""

echo "[PHASE-2] Running benchmark client (PATH B, Fresh Handshake)..."
./client_benchmark --path B

echo "✓ PATH B benchmark complete"

echo "[PHASE-2] Stopping worker_classic..."
kill $WORKER_CLASSIC_PID 2>/dev/null || true
sleep 1

echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# PHASE 3: Merge Results and Generate Plots
# ─────────────────────────────────────────────────────────────────────────── #

echo "═══════════════════════════════════════════════════════════════"
echo "  PHASE 3: Merging Results and Generating Plots"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo "[MERGE] Combining PATH A and PATH B results into single CSV..."

# Create combined results file with header
echo "path,gateway_us,worker_us,handshake_us,total_us" > results/mb3_1_results.csv

# Append PATH A results (gateway_us + worker_us)
if [ -f "results/mb3_1_gateway_timings.csv" ] && [ -f "results/mb3_1_worker_timings.csv" ]; then
    echo "[MERGE] Found PATH A timings (gateway + worker)"
    echo "[MERGE] Combining gateway and worker measurements..."
    
    # Paste gateway and worker columns, calculate totals
    paste -d ',' \
        <(sed 's/^/A,/' results/mb3_1_gateway_timings.csv) \
        results/mb3_1_worker_timings.csv \
        <(paste -d '+' results/mb3_1_gateway_timings.csv results/mb3_1_worker_timings.csv | bc) \
        >> results/mb3_1_results.csv
    
    echo "✓ PATH A results merged"
    
    # Count PATH A rows
    ROWS_A=$(wc -l < results/mb3_1_gateway_timings.csv)
    echo "  Total PATH A iterations: $ROWS_A"
else
    echo "WARNING: PATH A timing files not found"
fi

echo ""

# Append PATH B results (handshake_us)
if [ -f "results/mb3_1_handshake_timings.csv" ]; then
    echo "[MERGE] Found PATH B timings (handshake)"
    
    # Format PATH B as: path=B, gateway_us=0, worker_us=0, handshake_us=value, total_us=value
    while read handshake_us; do
        echo "B,0,0,$handshake_us,$handshake_us" >> results/mb3_1_results.csv
    done < results/mb3_1_handshake_timings.csv
    
    echo "✓ PATH B results merged"
    
    # Count PATH B rows
    ROWS_B=$(wc -l < results/mb3_1_handshake_timings.csv)
    echo "  Total PATH B iterations: $ROWS_B"
else
    echo "WARNING: PATH B timing file not found"
fi

echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# PHASE 4: Generate Comparison Plots
# ─────────────────────────────────────────────────────────────────────────── #

echo "[PLOT] Generating comparison visualizations..."

if [ -f "plot_mb3_1.py" ]; then
    python3 plot_mb3_1.py
    echo "✓ Plots generated: mb3_1_boxplot.png and mb3_1_boxplot.pdf"
else
    echo "WARNING: plot_mb3_1.py not found, skipping visualization"
fi

echo ""

# ─────────────────────────────────────────────────────────────────────────── #
# PHASE 5: Print Summary Statistics
# ─────────────────────────────────────────────────────────────────────────── #

echo "═══════════════════════════════════════════════════════════════"
echo "  SUMMARY STATISTICS"
echo "═══════════════════════════════════════════════════════════════"
echo ""

if [ -f "results/mb3_1_results.csv" ]; then
    echo "Combined results: results/mb3_1_results.csv"
    echo ""
    
    # PATH A statistics
    echo "PATH A: TLS Migration (Gateway + Worker)"
    echo "─────────────────────────────────────────"
    
    if [ -f "results/mb3_1_gateway_timings.csv" ]; then
        echo -n "Gateway (serialize + sendfd): "
        python3 -c "
import statistics
with open('results/mb3_1_gateway_timings.csv') as f:
    data = [float(line.strip()) for line in f]
print(f'Min={min(data):.0f}µs, Max={max(data):.0f}µs, Avg={statistics.mean(data):.0f}µs, Median={statistics.median(data):.0f}µs')
"
    fi
    
    if [ -f "results/mb3_1_worker_timings.csv" ]; then
        echo -n "Worker (receive + restore): "
        python3 -c "
import statistics
with open('results/mb3_1_worker_timings.csv') as f:
    data = [float(line.strip()) for line in f]
print(f'Min={min(data):.0f}µs, Max={max(data):.0f}µs, Avg={statistics.mean(data):.0f}µs, Median={statistics.median(data):.0f}µs')
"
    fi
    
    if [ -f "results/mb3_1_gateway_timings.csv" ] && [ -f "results/mb3_1_worker_timings.csv" ]; then
        echo -n "Total (gateway + worker): "
        python3 -c "
import statistics
with open('results/mb3_1_gateway_timings.csv') as f1, open('results/mb3_1_worker_timings.csv') as f2:
    gw = [float(line.strip()) for line in f1]
    wrk = [float(line.strip()) for line in f2]
    total = [g + w for g, w in zip(gw, wrk)]
print(f'Min={min(total):.0f}µs, Max={max(total):.0f}µs, Avg={statistics.mean(total):.0f}µs, Median={statistics.median(total):.0f}µs')
"
    fi
    
    echo ""
    
    # PATH B statistics
    echo "PATH B: Fresh TLS 1.3 Handshake"
    echo "─────────────────────────────────────"
    
    if [ -f "results/mb3_1_handshake_timings.csv" ]; then
        echo -n "Full Handshake: "
        python3 -c "
import statistics
with open('results/mb3_1_handshake_timings.csv') as f:
    data = [float(line.strip()) for line in f]
print(f'Min={min(data):.0f}µs, Max={max(data):.0f}µs, Avg={statistics.mean(data):.0f}µs, Median={statistics.median(data):.0f}µs')
"
    fi
    
    echo ""
    
    # Speedup calculation
    echo "Speedup Analysis"
    echo "─────────────────────────────────────"
    
    if [ -f "results/mb3_1_gateway_timings.csv" ] && [ -f "results/mb3_1_worker_timings.csv" ] && [ -f "results/mb3_1_handshake_timings.csv" ]; then
        python3 -c "
import statistics
with open('results/mb3_1_gateway_timings.csv') as f:
    gw = [float(line.strip()) for line in f]
with open('results/mb3_1_worker_timings.csv') as f:
    wrk = [float(line.strip()) for line in f]
with open('results/mb3_1_handshake_timings.csv') as f:
    hs = [float(line.strip()) for line in f]

migration_total = [g + w for g, w in zip(gw, wrk)]
speedup = [h / m for h, m in zip(hs, migration_total)]

print(f'Speedup: Min={min(speedup):.1f}x, Max={max(speedup):.1f}x, Avg={statistics.mean(speedup):.1f}x, Median={statistics.median(speedup):.1f}x')
print(f'  → TLS Migration is {statistics.median(speedup):.1f}x faster than fresh handshake')
"
    fi
    
    echo ""
fi

# ─────────────────────────────────────────────────────────────────────────── #
# FINAL STATUS
# ─────────────────────────────────────────────────────────────────────────── #

echo "═══════════════════════════════════════════════════════════════"
echo "  ✓ BENCHMARK COMPLETE"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Results:"
echo "  • Combined CSV:           results/mb3_1_results.csv"
echo "  • Gateway timings:        results/mb3_1_gateway_timings.csv"
echo "  • Worker timings:         results/mb3_1_worker_timings.csv"
echo "  • Handshake timings:      results/mb3_1_handshake_timings.csv"
echo "  • Comparison plots:       mb3_1_boxplot.png / .pdf"
echo ""
echo "See README.md for detailed interpretation of results."
echo ""
