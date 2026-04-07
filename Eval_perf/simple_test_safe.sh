#!/bin/bash

# SIMPLE TEST - Safe version (NO SUDO, NO AGGRESSIVE KILLS)

set -e

NUM_WORKERS=${1:-10}
DURATION=${2:-5}
CONCURRENCY=${3:-50}

echo "════════════════════════════════════════════════════════════"
echo " SIMPLE EVALUATION TEST (SAFE)"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "[*] Configuration:"
echo "    Workers: $NUM_WORKERS"
echo "    Duration: ${DURATION}s"
echo "    Concurrency: $CONCURRENCY"
echo ""

# Check prerequisites
echo "[*] Checking prerequisites..."
which wrk >/dev/null 2>&1 || { echo "ERROR: wrk not installed"; exit 1; }
[ -f ../libtlspeek/build/gateway ] || { echo "ERROR: gateway binary not found"; exit 1; }
[ -f ../libtlspeek/build/worker ] || { echo "ERROR: worker binary not found"; exit 1; }
echo "✓ All prerequisites present"
echo ""

# Set up environment
export LD_LIBRARY_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH

# Gentle cleanup function
cleanup_test() {
    echo "[*] Cleaning up processes..."
    pkill -f "direct_tls_server" 2>/dev/null || true
    pkill -f "proxy_worker" 2>/dev/null || true
    pkill -f "gateway" 2>/dev/null || true
    pkill -f "nginx" 2>/dev/null || true
    sleep 1
}

# ============================
# TEST 1: DIRECT MODE
# ============================
cleanup_test

echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 1: DIRECT MODE (Baseline)"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting direct server with $NUM_WORKERS listeners..."
./direct_tls_server 8445 $NUM_WORKERS > direct.log 2>&1 &
DIRECT_PID=$!
sleep 3

echo "[+] Running wrk benchmark (${DURATION}s)..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8445/function/hello 2>&1 | tee direct_bench.txt || true

if grep -q "Requests/sec" direct_bench.txt; then
    direct_rps=$(grep "Requests/sec" direct_bench.txt | awk '{print $2}')
    echo "[✓] DIRECT MODE: $direct_rps RPS"
else
    echo "[✗] DIRECT MODE: Benchmark failed"
    direct_rps=""
fi

echo "[+] Stopping direct server..."
kill $DIRECT_PID 2>/dev/null || true
sleep 1

# ============================
# TEST 2: NGINX PROXY MODE
# ============================
cleanup_test

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 2: NGINX PROXY MODE"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting $NUM_WORKERS proxy workers..."
WORKER_PIDS=()
for i in $(seq 0 $((NUM_WORKERS-1))); do
    ./proxy_worker $i > worker_${i}.log 2>&1 &
    WORKER_PIDS+=($!)
done
sleep 2

echo "[+] Starting nginx..."
bash manage_server.sh start nginx_proxy $NUM_WORKERS > nginx.log 2>&1 &
NGINX_PID=$!
sleep 3

echo "[+] Running wrk benchmark (${DURATION}s)..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8445/function/hello 2>&1 | tee nginx_bench.txt || true

if grep -q "Requests/sec" nginx_bench.txt; then
    nginx_rps=$(grep "Requests/sec" nginx_bench.txt | awk '{print $2}')
    echo "[✓] NGINX MODE: $nginx_rps RPS"
else
    echo "[✗] NGINX MODE: Benchmark failed"
    nginx_rps=""
fi

echo "[+] Stopping nginx and workers..."
kill $NGINX_PID 2>/dev/null || true
for pid in "${WORKER_PIDS[@]}"; do
    kill $pid 2>/dev/null || true
done
sleep 1

# ============================
# TEST 3: HOT POTATO MODE
# ============================
cleanup_test

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 3: HOT POTATO MODE"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting $NUM_WORKERS hot potato workers..."
WORKER_PIDS=()
for i in $(seq 0 $((NUM_WORKERS-1))); do
    ../libtlspeek/build/worker $i ../libtlspeek/certs/server.crt ../libtlspeek/certs/server.key > worker_${i}.log 2>&1 &
    WORKER_PIDS+=($!)
done
sleep 2

echo "[+] Starting gateway..."
../libtlspeek/build/gateway 8443 ../libtlspeek/certs/server.crt ../libtlspeek/certs/server.key $NUM_WORKERS > gateway.log 2>&1 &
GATEWAY_PID=$!
sleep 3

echo "[+] Running wrk benchmark (${DURATION}s)..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8443/function/hello 2>&1 | tee hotpotato_bench.txt || true

if grep -q "Requests/sec" hotpotato_bench.txt; then
    hotpotato_rps=$(grep "Requests/sec" hotpotato_bench.txt | awk '{print $2}')
    echo "[✓] HOT POTATO MODE: $hotpotato_rps RPS"
else
    echo "[✗] HOT POTATO MODE: Benchmark failed"
    hotpotato_rps=""
fi

echo "[+] Stopping gateway and workers..."
kill $GATEWAY_PID 2>/dev/null || true
for pid in "${WORKER_PIDS[@]}"; do
    kill $pid 2>/dev/null || true
done
sleep 1

# ============================
# FINAL SUMMARY
# ============================
cleanup_test

echo ""
echo "════════════════════════════════════════════════════════════"
echo " RESULTS SUMMARY"
echo "════════════════════════════════════════════════════════════"
echo ""

if [ -n "$direct_rps" ]; then
    echo "Direct Mode:     $direct_rps RPS (BASELINE)"
fi
if [ -n "$nginx_rps" ]; then
    echo "Nginx Mode:      $nginx_rps RPS"
fi
if [ -n "$hotpotato_rps" ]; then
    echo "Hot Potato Mode: $hotpotato_rps RPS"
fi

echo ""
echo "✓ Test completed. Results saved in:"
echo "  - direct_bench.txt"
echo "  - nginx_bench.txt"
echo "  - hotpotato_bench.txt"
echo ""
