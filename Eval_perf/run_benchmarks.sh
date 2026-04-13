#!/bin/bash

# Performance Evaluation Script
# Modes: Direct, Nginx, Hot Potato

RESULTS_FILE="results.csv"
echo "Mode,Connections,RPS,AvgLatency_ms,P99Latency_ms,MaxCPU_pct" > $RESULTS_FILE

CONCURRENCY=(50 100 300 500 700 900 1000)
DURATION=20s
THREADS=2
URL="https://localhost:8443/function/hello"
CERT_DIR="../libtlspeek/certs"

# Increase file descriptors
ulimit -n 65535

# Ensure we use the custom wolfSSL library
export LD_LIBRARY_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH

cleanup() {
    echo "Cleaning up..."
    pkill -9 direct_server
    pkill -9 plain_worker
    pkill -9 nginx
    pkill -9 gateway
    pkill -9 worker
    rm -f /tmp/nginx_worker.sock /tmp/worker_*.sock
    sleep 2
}

run_wrk() {
    local mode=$1
    local conn=$2
    echo "Running benchmark for $mode with $conn connections..."
    
    # Start CPU monitoring in background
    # We sample CPU for the duration of the test
    PID_MONITOR=$(pgrep -f $mode | head -n 1)
    if [ -z "$PID_MONITOR" ]; then
        # For Hot Potato, we monitor the Gateway
        PID_MONITOR=$(pgrep -f "gateway" | head -n 1)
    fi
    
    # Output file for wrk
    WRK_OUT="wrk_${mode}_${conn}.txt"
    
    # Run wrk
    wrk --latency -d $DURATION -c $conn -t $THREADS $URL > $WRK_OUT 2>&1 &
    WRK_PID=$!
    
    # Measure CPU while wrk is running
    # Using pidstat to get average CPU during the test
    CPU_VAL=0
    if [ ! -z "$PID_MONITOR" ]; then
        # Sample for 10 seconds during the middle of the run
        CPU_VAL=$(pidstat -u -p $PID_MONITOR 10 1 | tail -n 1 | awk '{print $8}' | sed 's/,/./')
    fi
    
    wait $WRK_PID
    
    # Extract metrics
    RPS=$(grep "Requests/sec:" $WRK_OUT | awk '{print $2}')
    LAT_AVG=$(grep "Latency" $WRK_OUT | grep -v "Distribution" | awk '{print $2}')
    LAT_P99=$(grep "99%" $WRK_OUT | awk '{print $2}')
    
    # Convert latency to ms if in us
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
    
    LAT_AVG_MS=$(convert_to_ms $LAT_AVG)
    LAT_P99_MS=$(convert_to_ms $LAT_P99)
    
    echo "$mode,$conn,$RPS,$LAT_AVG_MS,$LAT_P99_MS,$CPU_VAL" >> $RESULTS_FILE
    echo "Done: RPS=$RPS, Latency=$LAT_AVG_MS ms, CPU=$CPU_VAL%"
}

# 1. DIRECT MODE
cleanup
./direct_server 8443 > direct.log 2>&1 &
sleep 2
for c in "${CONCURRENCY[@]}"; do
    run_wrk "direct_server" $c
done

# 2. NGINX MODE
cleanup
./plain_worker > plain_worker.log 2>&1 &
nginx -c $(pwd)/nginx.conf -g "daemon off;" > nginx.log 2>&1 &
sleep 2
for c in "${CONCURRENCY[@]}"; do
    run_wrk "nginx" $c
done

# 3. HOT POTATO MODE
cleanup
cd ../libtlspeek/build
./worker 0 ../certs/server.crt ../certs/server.key > worker0.log 2>&1 &
./worker 1 ../certs/server.crt ../certs/server.key > worker1.log 2>&1 &
sleep 1
./gateway 8443 ../certs/server.crt ../certs/server.key 2 > gateway.log 2>&1 &
cd ../../Eval_perf
sleep 2
for c in "${CONCURRENCY[@]}"; do
    run_wrk "gateway" $c
done

cleanup
echo "Benchmarks completed. Results saved to $RESULTS_FILE"
