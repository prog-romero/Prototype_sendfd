#!/bin/bash

# SIMPLE TEST - Verify basic setup works before running full evaluation

echo "════════════════════════════════════════════════════════════"
echo " SIMPLE EVALUATION TEST"
echo "════════════════════════════════════════════════════════════"
echo ""

NUM_WORKERS=${1:-10}
DURATION=${2:-5}
CONCURRENCY=${3:-50}

echo "[*] Configuration:"
echo "    Workers: $NUM_WORKERS"
echo "    Duration: ${DURATION}s"
echo "    Concurrency: $CONCURRENCY"
echo ""

# Check prerequisites
echo "[*] Checking prerequisites..."
which wrk >/dev/null 2>&1 || { echo "ERROR: wrk not installed"; exit 1; }
which nginx >/dev/null 2>&1 || { echo "ERROR: nginx not installed"; exit 1; }
[ -f ../libtlspeek/build/gateway ] || { echo "ERROR: gateway binary not found"; exit 1; }
[ -f ../libtlspeek/build/worker ] || { echo "ERROR: worker binary not found"; exit 1; }
[ -f direct_tls_server ] || gcc -Wall -O2 direct_tls_server.c ../libtlspeek/worker/handler.c -o direct_tls_server \
    -I../libtlspeek/common -I../libtlspeek/worker \
    -I../wolfssl \
    -L../wolfssl/src/.libs \
    -lwolfssl 2>/dev/null
[ -f proxy_worker ] || gcc -Wall -O2 proxy_worker.c -o proxy_worker 2>/dev/null
echo "✓ All prerequisites present"
echo ""

# Clean up any old processes
echo "[*] Cleaning up old processes..."
pkill -f "gateway|worker|nginx|direct_tls_server|proxy_worker" 2>/dev/null || true
sleep 1

# Set up environment
export LD_LIBRARY_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH

# Test 1: DIRECT MODE
echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 1: DIRECT MODE (Baseline)"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting direct server with $NUM_WORKERS listeners..."
./direct_tls_server 8445 $NUM_WORKERS > direct.log 2>&1 &
DIRECT_PID=$!
sleep 3

echo "[+] Running wrk benchmark..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8445/function/hello 2>&1 | tee direct_bench.txt

if grep -q "Requests/sec" direct_bench.txt; then
    direct_rps=$(grep "Requests/sec" direct_bench.txt | awk '{print $2}')
    echo "[✓] DIRECT MODE: $direct_rps RPS"
else
    echo "[✗] DIRECT MODE: No results"
fi

echo "[+] Stopping..."
kill $DIRECT_PID 2>/dev/null || true
sleep 1

# Test 2: NGINX PROXY MODE
echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 2: NGINX PROXY MODE"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting $NUM_WORKERS proxy workers..."
for i in $(seq 0 $((NUM_WORKERS-1))); do
    ./proxy_worker $i > worker_${i}.log 2>&1 &
done
sleep 2

echo "[+] Starting nginx..."
bash manage_server.sh start nginx_proxy $NUM_WORKERS > nginx.log 2>&1
sleep 3

echo "[+] Running wrk benchmark..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8445/function/hello 2>&1 | tee nginx_bench.txt

if grep -q "Requests/sec" nginx_bench.txt; then
    nginx_rps=$(grep "Requests/sec" nginx_bench.txt | awk '{print $2}')
    echo "[✓] NGINX MODE: $nginx_rps RPS"
else
    echo "[✗] NGINX MODE: No results"
fi

echo "[+] Stopping..."
pkill -f "nginx|proxy_worker" 2>/dev/null || true
sleep 1

# Test 3: HOT POTATO MODE
echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║ TEST 3: HOT POTATO MODE"
echo "╚════════════════════════════════════════════════════════════╝"
echo "[+] Starting $NUM_WORKERS hot potato workers..."
for i in $(seq 0 $((NUM_WORKERS-1))); do
    ../libtlspeek/build/worker $i ../libtlspeek/certs/server.crt ../libtlspeek/certs/server.key > worker_${i}.log 2>&1 &
done
sleep 2

echo "[+] Starting gateway..."
../libtlspeek/build/gateway 8443 ../libtlspeek/certs/server.crt ../libtlspeek/certs/server.key $NUM_WORKERS > gateway.log 2>&1 &
sleep 3

echo "[+] Running wrk benchmark..."
wrk -t 4 -c $CONCURRENCY -d ${DURATION}s --latency https://127.0.0.1:8443/function/hello 2>&1 | tee hotpotato_bench.txt

if grep -q "Requests/sec" hotpotato_bench.txt; then
    hotpotato_rps=$(grep "Requests/sec" hotpotato_bench.txt | awk '{print $2}')
    echo "[✓] HOT POTATO MODE: $hotpotato_rps RPS"
else
    echo "[✗] HOT POTATO MODE: No results"
fi

echo "[+] Stopping..."
pkill -f "gateway|worker" 2>/dev/null || true
sleep 1

# Final Summary
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
    if [ -n "$direct_rps" ]; then
        overhead=$(echo "scale=1; (($direct_rps - $nginx_rps) / $direct_rps) * 100" | bc)
        echo "                 Overhead: $overhead%"
    fi
fi
if [ -n "$hotpotato_rps" ]; then
    echo "Hot Potato Mode: $hotpotato_rps RPS"
    if [ -n "$direct_rps" ]; then
        overhead=$(echo "scale=1; (($direct_rps - $hotpotato_rps) / $direct_rps) * 100" | bc)
        echo "                 Overhead: $overhead%"
    fi
fi

echo ""
if [ "$direct_rps" -gt "$nginx_rps" ] 2>/dev/null && [ "$direct_rps" -gt "$hotpotato_rps" ] 2>/dev/null; then
    echo "✓ TEST PASSED: Direct mode is fastest (as expected)"
else
    echo "✗ TEST FAILED: Check server logs"
fi

echo ""
echo "Benchmark outputs saved:"
echo "  - direct_bench.txt"
echo "  - nginx_bench.txt"
echo "  - hotpotato_bench.txt"
echo ""
echo "Server logs:"
echo "  - direct.log"
echo "  - nginx.log"
echo "  - gateway.log"
echo "  - worker_*.log"
