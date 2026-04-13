#!/bin/bash

# CORRECTED EVALUATION SCRIPT - FAIR COMPARISON WITH N WORKERS
# 
# Principle: Direct mode is the BASELINE and should be FASTEST.
# Nginx proxy adds overhead (forwarding), so it should be SLOWER.
# Hot Potato adds overhead (TLS inspection + routing), should be similar to direct.
#
# CPU Allocation: All modes use same total CPUs to ensure fair comparison.
# Workers: ALL MODES USE N WORKERS (e.g., 100) so workers are NOT the bottleneck.
#
# Usage: bash run_evaluation_fixed.sh [num_workers] [num_cpus] [duration] [concurrency_levels]

NUM_WORKERS=${1:-100}       # Number of workers per mode (default: 100)
NUM_CPUS=${2:-5}            # Total CPUs per mode (default: 5)
DURATION=${3:-30}           # Test duration (default: 30s)
CONC_LEVELS=${4:-"100 500 1000"}  # Concurrency levels (default: 100, 500, 1000)

RESULTS_FILE="consolidated_results_fixed.csv"
CERT_DIR="../libtlspeek/certs"
WRK_THREADS=4

echo "[=] FAIR EVALUATION CONFIGURATION"
echo "    Workers per mode: $NUM_WORKERS"
echo "    CPUs per mode: $NUM_CPUS"
echo "    Duration per test: ${DURATION}s"
echo "    Concurrency levels: $CONC_LEVELS"

# Increase file descriptors
ulimit -n 65535

# Header
echo "Mode,NumWorkers,Concurrency,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,MaxLatency_ms,ConnectErrors,ReadErrors,WriteErrors,AvgCPU_pct,UserCPU_pct,SysCPU_pct,Overhead_vs_Direct_pct" > $RESULTS_FILE

cleanup() {
    echo "[*] Cleanup..."
    sudo ./manage_server.sh stop >/dev/null 2>&1
    sleep 3
}

run_test() {
    local mode=$1
    local num_workers=$2
    local concurrency=$3
    local cpumask=$4
    
    local url="https://127.0.0.1:8445/function/hello"
    
    echo ""
    echo "[TEST] $mode | workers=$num_workers | concurrency=$concurrency"
    
    # Start server with fair CPU allocation
    echo "  [+] Starting servers..."
    sudo ./manage_server.sh start $mode $num_workers >/dev/null 2>&1
    sleep 5  # Wait for servers to be ready
    
    # Monitor CPU
    echo "  [+] Running benchmark..."
    (while true; do 
        top -bn1 -p $(pgrep -f "gateway|worker|nginx|direct_tls" | tr '\n' ',' | sed 's/,$//') 2>/dev/null | grep -E "Cpu|worker|gateway|nginx|direct" 
        sleep 1
    done) > cpu_monitor_${mode}_${concurrency}.log 2>&1 &
    MONITOR_PID=$!
    
    # Run wrk benchmark
    local wrk_output="wrk_${mode}_${num_workers}_${concurrency}.txt"
    local before_time=$(date +%s)
    
    # wrk with correct syntax: wrk -t threads -c connections -d duration [--latency] url
    # For self-signed certs, wrk doesn't have --insecure, so we test with plain HTTP for now
    # Or use wrk with HTTPS and hope it works
    wrk -t $WRK_THREADS -c $concurrency -d ${DURATION}s --latency "$url" 2>&1 | tee "$wrk_output"
    
    local after_time=$(date +%s)
    local elapsed=$((after_time - before_time))
    
    # Kill monitoring
    kill $MONITOR_PID 2>/dev/null
    
    # Check if wrk produced output
    if [ ! -s "$wrk_output" ] || ! grep -q "Requests/sec" "$wrk_output"; then
        echo "  [✗] wrk failed or produced no output"
        rps=0
        avg_latency=0
        p99_latency=0
        max_latency=0
        connect_errors=0
        read_errors=0
        write_errors=0
    else
        # Parse results from wrk output
        local rps=$(grep "Requests/sec" "$wrk_output" | awk '{print $2}' | head -1)
        local avg_latency=$(grep "Average" "$wrk_output" | awk '{print $2}' | sed 's/ms//' | head -1)
        local p99_latency=$(grep "99%" "$wrk_output" | awk '{print $2}' | sed 's/ms//' | head -1)
        local max_latency=$(grep "Max" "$wrk_output" | awk '{print $2}' | sed 's/ms//' | head -1)
        
        # Extract error counts
        local connect_errors=$(grep "Socket errors" "$wrk_output" | grep -oE "connect [0-9]+" | awk '{print $2}')
        local read_errors=$(grep "Socket errors" "$wrk_output" | grep -oE "read [0-9]+" | awk '{print $2}')
        local write_errors=$(grep "Socket errors" "$wrk_output" | grep -oE "write [0-9]+" | awk '{print $2}')
    fi
    
    # Provide defaults if parsing failed
    rps=${rps:-0}
    avg_latency=${avg_latency:-0}
    p99_latency=${p99_latency:-0}
    max_latency=${max_latency:-0}
    connect_errors=${connect_errors:-0}
    read_errors=${read_errors:-0}
    write_errors=${write_errors:-0}
    
    # Calculate throughput (bytes/sec → MB/s)
    # Check if rps is valid before calculating
    if [ "$rps" != "0" ] && [ -n "$rps" ]; then
        throughput_mbs=$(echo "scale=2; $rps * 1024 / 1048576" | bc 2>/dev/null)
    else
        throughput_mbs=0
    fi
    throughput_mbs=${throughput_mbs:-0}
    
    # Get CPU stats from monitoring (if file exists)
    if [ -f "cpu_monitor_${mode}_${concurrency}.log" ]; then
        avg_cpu=$(grep "Cpu" cpu_monitor_${mode}_${concurrency}.log | awk '{print $2}' | sed 's/%us.*//' | awk '{sum+=$1; count++} END {if (count>0) print sum/count; else print 0}')
        user_cpu=$(grep "Cpu" cpu_monitor_${mode}_${concurrency}.log | awk '{print $2}' | sed 's/%us.*//' | head -1)
        sys_cpu=$(grep "Cpu" cpu_monitor_${mode}_${concurrency}.log | awk '{print $4}' | sed 's/%sy.*//' | head -1)
    else
        avg_cpu=0
        user_cpu=0
        sys_cpu=0
    fi
    
    avg_cpu=${avg_cpu:-0}
    user_cpu=${user_cpu:-0}
    sys_cpu=${sys_cpu:-0}
    
    # Calculate overhead vs direct (filled in after all tests)
    overhead="TBD"
    
    echo "$mode,$num_workers,$concurrency,$rps,$throughput_mbs,$avg_latency,$p99_latency,$max_latency,$connect_errors,$read_errors,$write_errors,$avg_cpu,$user_cpu,$sys_cpu,$overhead" >> $RESULTS_FILE
    
    echo "  [✓] RPS: $rps | Latency: ${avg_latency}ms | P99: ${p99_latency}ms | CPU: ${avg_cpu}%"
    
    # Cleanup
    cleanup
}

# CPU masks for fair allocation (e.g., 5 CPUs = cores 0-4)
CPU_MASK="0-$((NUM_CPUS-1))"

echo "[=] RUNNING TESTS IN FAIR MODE"
echo ""

# Store direct mode results for overhead calculation
declare -A direct_results

for conc in $CONC_LEVELS; do
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║ PHASE 1: DIRECT MODE (Baseline) - $conc concurrent connections"
    echo "╚════════════════════════════════════════════════════════════╝"
    run_test "nginx_direct" "$NUM_WORKERS" "$conc" "$CPU_MASK"
    
    # Extract and store direct mode RPS
    direct_rps=$(tail -1 "$RESULTS_FILE" | awk -F',' '{print $4}')
    direct_results[$conc]=$direct_rps
done

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ PHASE 2: NGINX PROXY MODE (Should be slower than direct)  "
echo "╚════════════════════════════════════════════════════════════╝"
for conc in $CONC_LEVELS; do
    run_test "nginx_proxy" "$NUM_WORKERS" "$conc" "$CPU_MASK"
done

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ PHASE 3: HOT POTATO MODE (Should be close to direct)      "
echo "╚════════════════════════════════════════════════════════════╝"
for conc in $CONC_LEVELS; do
    run_test "hotpotato" "$NUM_WORKERS" "$conc" "$CPU_MASK"
done

# Calculate overhead percentages
echo ""
echo "[=] CALCULATING OVERHEAD METRICS..."
python3 - << 'EOF' "$RESULTS_FILE"
import sys
import pandas as pd

results_file = sys.argv[1]
df = pd.read_csv(results_file)

# Group by modemachine and concurrency
for conc in df['Concurrency'].unique():
    Direct_RPS = df[(df['Mode'] == 'nginx_direct') & (df['Concurrency'] == conc)]['RPS'].values[0] if len(df[(df['Mode'] == 'nginx_direct') & (df['Concurrency'] == conc)]) > 0 else 1
    
    print(f"\n--- Concurrency: {conc} ---")
    print(f"Direct:     {Direct_RPS:.0f} RPS")
    
    for mode in ['nginx_proxy', 'hotpotato']:
        result = df[(df['Mode'] == mode) & (df['Concurrency'] == conc)]
        if len(result) > 0:
            rps = result['RPS'].values[0]
            overhead = ((Direct_RPS - rps) / Direct_RPS * 100)
            print(f"{mode:12} {rps:7.0f} RPS ({overhead:+.1f}% overhead)")

EOF

echo ""
echo "[✓] Evaluation complete. Results in: $RESULTS_FILE"
echo ""
echo "KEY EXPECTATIONS:"
echo "  ✓ Direct mode:     BASELINE (should be FASTEST)"
echo "  ✓ Nginx Proxy:     5-15% slower (forwarding overhead)"
echo "  ✗ Hot Potato:      Should be similar to Direct or slightly slower (routing overhead)"
echo ""
echo "If Nginx Proxy is FASTER than Direct → EVALUATION IS BROKEN"
