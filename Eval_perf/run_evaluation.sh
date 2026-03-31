#!/bin/bash

# Automated Evaluation Orchestrator (Server Side - Port 8445)
# This script guides the user through each mode, monitors CPU, and triggers plotting.

RESULTS_FILE="consolidated_results.csv"
echo "Mode,Connections,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,ConnectErrors,ReadErrors,WriteErrors,Timeouts,AvgCPU_pct" > $RESULTS_FILE

run_phase() {
    local mode=$1
    local name=$2
    local path=$3
    
    echo "=================================================="
    echo "Phase: $name"
    echo "[*] Cleaning up logs and sockets..."
    sudo rm -f *.log *.sock /tmp/nginx.pid
    
    sudo ./manage_server.sh start $mode
    
    # Start CPU monitoring (background)
    if [ "$mode" == "hotpotato" ]; then
        MONITOR_PATTERN="gateway|worker"
    elif [ "$mode" == "nginx_proxy" ]; then
        MONITOR_PATTERN="nginx|proxy_worker"
    else
        MONITOR_PATTERN="nginx"
    fi
    
    echo "[*] Server IS READY on Port 8445."
    echo "[!] On your CLIENT machine, run: bash remote_bench.sh SERVER_IP $mode $path"
    echo "[*] Measuring CPU usage in background..."
    
    # Sample CPU every 2 seconds
    rm -f "/tmp/cpu_${mode}.log"
    (
        while true; do
            # Sum CPU of all matching processes
            SAMPLE=$(ps aux | grep -v grep | grep -E "$MONITOR_PATTERN" | awk '{sum+=$3} END {print sum}')
            if [ ! -z "$SAMPLE" ]; then
                echo "$SAMPLE" >> "/tmp/cpu_${mode}.log"
            fi
            sleep 2
        done
    ) &
    MONITOR_PID=$!
    
    read -p "[?] Press Enter here once the benchmark for $mode is finished on the Client..."
    
    kill $MONITOR_PID
    sudo ./manage_server.sh stop
    
    # Calculate Avg CPU
    if [ -f "/tmp/cpu_${mode}.log" ]; then
        AVG_CPU=$(awk '{sum+=$1; count++} END {if (count > 0) print sum/count; else print 0}' "/tmp/cpu_${mode}.log")
    else
        AVG_CPU=0
    fi
    echo "[*] Average Server CPU during Phase: $AVG_CPU%"
    
    # Wait for the user to have the CSV ready (results_hotpotato.csv, etc.)
    CSV_NAME="results_${mode}.csv"
    while [ ! -f "$CSV_NAME" ]; do
        echo "[!] $CSV_NAME NOT FOUND in this directory."
        read -p "[!] Please copy it from the Client machine and press Enter..."
    done
    
    # Append results to consolidated file with CPU data
    tail -n +2 "$CSV_NAME" | while read -r line; do
        # Extract columns up to Timeouts, then append CPU
        echo "$line,$AVG_CPU" >> $RESULTS_FILE
    done
}

# Run Phases
run_phase "nginx_direct" "Nginx Direct (Baseline)" "/direct"
run_phase "nginx_proxy"  "Nginx Proxy (Standard)"  "/"
run_phase "hotpotato"    "Hot Potato (Prototype)" "/function/hello"

echo "=================================================="
echo "ALL MODES COMPLETED."
echo "[*] Generating final plots..."
python3 generate_plots.py
echo "[*] Success. Plots: throughput_rps.png, throughput_mbs.png, latency.png, errors.png, cpu_usage.png"
