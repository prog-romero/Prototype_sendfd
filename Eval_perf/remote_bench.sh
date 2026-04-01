#!/bin/bash

# Remote Benchmarking Script (Client Side)
# Usage: ./remote_bench.sh <SERVER_IP> <MODE_NAME> <PATH>

SERVER_IP=$1
MODE=$2
URL_PATH=$3

if [ -z "$SERVER_IP" ] || [ -z "$MODE" ] || [ -z "$URL_PATH" ]; then
    echo "Usage: $0 <SERVER_IP> <MODE_NAME> <PATH>"
    echo "Example: ./remote_bench.sh 172.20.10.2 nginx_direct /direct"
    exit 1
fi

URL="https://${SERVER_IP}:8445${URL_PATH}"
# Expanded concurrency sampling (15 points up to 1000)
CONCURRENCY=(50 100 150 200 250 300 350 400 500 600 700 800 900 950 1000)
DURATION="30s"
THREADS=4 # Increased threads for client-side capacity

RESULTS_FILE="results_${MODE}.csv"
echo "Mode,Connections,RPS,Throughput_MBs,AvgLatency_ms,P99Latency_ms,ConnectErrors,ReadErrors,WriteErrors,Timeouts" > $RESULTS_FILE

echo "Starting isolated benchmarks for $MODE against $SERVER_IP..."
echo "URL: $URL"

# Helper functions
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

for conn in "${CONCURRENCY[@]}"; do
    echo "--------------------------------------------------"
    echo "[*] Testing with $conn connections..."
    
    # Isolated run: sleep before test to ensure no noise from previous run
    sleep 5
    
    # Run wrk
    WRK_OUT=$(wrk --latency -d $DURATION -c $conn -t $THREADS $URL)
    echo "$WRK_OUT"
    
    # Extract metrics
    RPS=$(echo "$WRK_OUT" | grep "Requests/sec:" | awk '{print $2}')
    TRANSFER=$(echo "$WRK_OUT" | grep "Transfer/sec:" | awk '{print $2}')
    LAT_AVG=$(echo "$WRK_OUT" | grep "Latency" | grep -v "Distribution" | awk '{print $2}')
    LAT_P99=$(echo "$WRK_OUT" | grep "99%" | awk '{print $2}')
    
    # Extract socket errors
    ERR_LINE=$(echo "$WRK_OUT" | grep "Socket errors:")
    if [ -z "$ERR_LINE" ]; then
        CON_ERR=0; READ_ERR=0; WRITE_ERR=0; TO_ERR=0
    else
        CON_ERR=$(echo "$ERR_LINE" | awk -F'connect ' '{print $2}' | awk -F',' '{print $1}')
        READ_ERR=$(echo "$ERR_LINE" | awk -F'read ' '{print $2}' | awk -F',' '{print $1}')
        WRITE_ERR=$(echo "$ERR_LINE" | awk -F'write ' '{print $2}' | awk -F',' '{print $1}')
        TO_ERR=$(echo "$ERR_LINE" | awk -F'timeout ' '{print $2}' | awk -F',' '{print $1}')
    fi
    
    # Handle empty/missing values
    THROUGHPUT_MBS=$(convert_to_mbs ${TRANSFER:-0B})
    LAT_AVG_MS=$(convert_to_ms ${LAT_AVG:-0ms})
    LAT_P99_MS=$(convert_to_ms ${LAT_P99:-0ms})
    
    echo "$MODE,$conn,$RPS,$THROUGHPUT_MBS,$LAT_AVG_MS,$LAT_P99_MS,$CON_ERR,$READ_ERR,$WRITE_ERR,$TO_ERR" >> $RESULTS_FILE
done

echo "--------------------------------------------------"
echo "Benchmarks completed for $MODE."
echo "Results saved to $RESULTS_FILE"
