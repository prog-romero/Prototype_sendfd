#!/bin/bash

# Automated Evaluation Orchestrator (Fully Local)
# Runs Servers, CPU Monitoring, and wrk benchmarks on the same machine.

RESULTS_FILE="consolidated_results.csv"
echo "Mode,Connections,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,ConnectErrors,ReadErrors,WriteErrors,Timeouts,AvgCPU_pct" > $RESULTS_FILE

convert_to_mbs() {
    local val=$1
    if [[ $val == *GB ]]; then
        echo "scale=2; ${val%GB} * 1024" | bc
    elif [[ $val == *MB ]]; then
        echo "${val%MB}"
    elif [[ $val == *KB ]]; then
        echo "scale=3; ${val%KB} / 1024" | bc
    elif [[ $val == *B ]]; then
        echo "scale=6; ${val%B} / 1048576" | bc
    else
        echo "0.0"
    fi
}

convert_to_ms() {
    local val=$1
    if [[ $val == *us ]]; then
        echo "scale=3; ${val%us} / 1000" | bc
    elif [[ $val == *ms ]]; then
        echo "${val%ms}"
    elif [[ $val == *s ]]; then
        echo "scale=3; ${val%s} * 1000" | bc
    else
        echo "$val"
    fi
}

run_phase() {
    local mode=$1
    local name=$2
    local path=$3
    
    echo "=================================================="
    echo "Phase: $name"
    echo "[*] Ensuring clean slate... stopping servers."
    sudo ./manage_server.sh stop
    sleep 5 # Cooldown to clear noise
    
    # Increase local limits for wrk client as well
    ulimit -n 65535
    
    sudo ./manage_server.sh start $mode
    echo "[*] Waiting a few seconds for server startup..."
    sleep 3
    
    if [ "$mode" == "hotpotato" ]; then
        MONITOR_PATTERN="gateway|worker"
    elif [ "$mode" == "nginx_proxy" ]; then
        MONITOR_PATTERN="nginx|proxy_worker"
    else
        MONITOR_PATTERN="direct_tls_server"
    fi
    
    echo "[*] Server IS READY. Starting CPU Monitor..."
    rm -f "/tmp/cpu_${mode}.log"
    (
        while true; do
            SAMPLE=$(ps aux | grep -v grep | grep -E "$MONITOR_PATTERN" | awk '{sum+=$3} END {print sum}')
            if [ ! -z "$SAMPLE" ]; then
                echo "$SAMPLE" >> "/tmp/cpu_${mode}.log"
            fi
            sleep 2
        done
    ) &
    MONITOR_PID=$!
    
    URL="https://127.0.0.1:8445${path}"
    CONCURRENCY=(50 100 150 200 250 300 350 400 500 600 700 800 900 950 1000)
    DURATION="10s"
    THREADS=4
    
    CSV_NAME="results_${mode}.csv"
    echo "Mode,Connections,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,ConnectErrors,ReadErrors,WriteErrors,Timeouts" > $CSV_NAME
    
    for conn in "${CONCURRENCY[@]}"; do
        echo "--------------------------------------------------"
        echo "[*] Testing $mode with $conn connections..."
        sleep 5 # Wait before test to avoid immediate connection burst issues
        
        WRK_OUT=$(wrk --latency -d $DURATION -c $conn -t $THREADS $URL)
        echo "$WRK_OUT"
        
        RPS=$(echo "$WRK_OUT" | grep "Requests/sec:" | awk '{print $2}')
        TRANSFER=$(echo "$WRK_OUT" | grep "Transfer/sec:" | awk '{print $2}')
        LAT_AVG=$(echo "$WRK_OUT" | grep "Latency" | grep -v "Distribution" | awk '{print $2}')
        LAT_P99=$(echo "$WRK_OUT" | grep "99%" | awk '{print $2}')
        
        ERR_LINE=$(echo "$WRK_OUT" | grep "Socket errors:")
        if [ -z "$ERR_LINE" ]; then
            CON_ERR=0; READ_ERR=0; WRITE_ERR=0; TO_ERR=0
        else
            CON_ERR=$(echo "$ERR_LINE" | awk -F'connect ' '{print $2}' | awk -F',' '{print $1}')
            READ_ERR=$(echo "$ERR_LINE" | awk -F'read ' '{print $2}' | awk -F',' '{print $1}')
            WRITE_ERR=$(echo "$ERR_LINE" | awk -F'write ' '{print $2}' | awk -F',' '{print $1}')
            TO_ERR=$(echo "$ERR_LINE" | awk -F'timeout ' '{print $2}' | awk -F',' '{print $1}')
        fi
        
        THROUGHPUT_MBS=$(convert_to_mbs ${TRANSFER:-0B})
        LAT_AVG_MS=$(convert_to_ms ${LAT_AVG:-0ms})
        LAT_P99_MS=$(convert_to_ms ${LAT_P99:-0ms})
        
        echo "$mode,$conn,$RPS,$THROUGHPUT_MBS,$LAT_AVG_MS,$LAT_P99_MS,$CON_ERR,$READ_ERR,$WRITE_ERR,$TO_ERR" >> $CSV_NAME
    done
    
    echo "[*] Stopping CPU Monitor and Servers..."
    kill $MONITOR_PID
    sudo ./manage_server.sh stop
    sleep 5 # Cooldown before next phase
    
    if [ -f "/tmp/cpu_${mode}.log" ]; then
        AVG_CPU=$(awk '{sum+=$1; count++} END {if (count > 0) print sum/count; else print 0}' "/tmp/cpu_${mode}.log")
    else
        AVG_CPU=0
    fi
    echo "[*] Average Server CPU during Phase ($mode): ${AVG_CPU}%"
    
    tail -n +2 "$CSV_NAME" | while read -r line; do
        echo "$line,$AVG_CPU" >> $RESULTS_FILE
    done
}

# Run Phases automatically
run_phase "nginx_direct" "Direct TLS (Baseline)" "/sum?a=10&b=20"
run_phase "nginx_proxy"  "Nginx Proxy (Standard)"  "/sum?a=10&b=20"
run_phase "hotpotato"    "Hot Potato (Prototype)" "/sum?a=10&b=20"

echo "=================================================="
echo "ALL MODES COMPLETED."
echo "[*] Generating final plots..."
python3 generate_plots.py
echo "[*] Success. Plots generated!"
