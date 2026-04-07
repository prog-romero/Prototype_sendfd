#!/bin/bash

# FIXED Server Management Script - Fair Evaluation with N Workers
# Usage: sudo ./manage_server.sh [start|stop] [mode] [num_workers]
# Example: sudo ./manage_server.sh start nginx_direct 100

ACTION=$1
MODE=$2
NUM_WORKERS=${3:-100}  # Default: 100 workers for fair comparison

export LD_LIBRARY_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH

# Increase File Descriptor limits for high concurrency
ulimit -n 65535

cleanup() {
    echo "[*] Cleaning up logs and sockets..."
    rm -f *.log *.sock worker_*.sock
    echo "[*] Cleaning up old processes..."
    # Use exact match or explicit paths to avoid suicidal kill
    pkill -x gateway
    pkill -x worker
    pkill -x proxy_worker
    pkill -x direct_tls_server
    sudo pkill -x nginx
    sleep 2
}

start_proxy_workers() {
    local num=$1
    echo "[+] Starting $num Proxy Workers (Unix sockets)..."
    gcc -Wall -O2 proxy_worker.c -o proxy_worker 2>/dev/null
    
    for i in $(seq 0 $((num-1))); do
        ./proxy_worker $i > worker_${i}.log 2>&1 &
    done
    sleep 2
}

start_hotpotato_workers() {
    local num=$1
    echo "[+] Starting $num Hot Potato Workers..."
    BUILD_DIR="/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build"
    CERT="../libtlspeek/certs/server.crt"
    KEY="../libtlspeek/certs/server.key"
    
    for i in $(seq 0 $((num-1))); do
        $BUILD_DIR/worker $i $CERT $KEY > worker_${i}.log 2>&1 &
    done
    sleep 2
}

generate_nginx_upstream() {
    local num=$1
    local output_file="nginx_upstream.conf"
    
    # Calculate keepalive connections (should be >= num_workers * nginx_worker_processes)
    # Assuming 4 nginx worker processes, use num * 4 but minimum 256
    local keepalive=$((num > 64 ? num * 4 : 256))
    
    echo "# Auto-generated upstream configuration for $num workers" > "$output_file"
    echo "# keepalive = $keepalive (scales with number of workers)" >> "$output_file"
    echo "upstream backend {" >> "$output_file"
    echo "    keepalive $keepalive;" >> "$output_file"
    
    for i in $(seq 0 $((num-1))); do
        echo "    server unix:$(pwd)/worker_${i}_proxy.sock max_fails=0;" >> "$output_file"
    done
    
    echo "}" >> "$output_file"
    echo "[*] Generated nginx_upstream.conf with $num workers (keepalive=$keepalive)"
}

if [ "$ACTION" == "stop" ]; then
    cleanup
    echo "[*] All servers stopped."
    exit 0
fi

echo "[=] Fair Evaluation Setup: $NUM_WORKERS workers per mode"

case "$MODE" in
    nginx_direct)
        cleanup
        echo "[+] Starting Direct TLS Server with $NUM_WORKERS listener processes..."
        gcc -Wall -O2 direct_tls_server.c ../libtlspeek/worker/handler.c -o direct_tls_server \
            -I../libtlspeek/common -I../libtlspeek/worker \
            -I/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl \
            -L/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs \
            -lwolfssl -DNUM_LISTENERS=$NUM_WORKERS 2>/dev/null
        ./direct_tls_server 8445 $NUM_WORKERS > direct_server.log 2>&1 &
        sleep 2
        ;;
    nginx_proxy)
        cleanup
        generate_nginx_upstream $NUM_WORKERS
        start_proxy_workers $NUM_WORKERS
        echo "[+] Starting Nginx in PROXY mode on port 8445..."
        # Replace placeholder in nginx.conf with generated upstream config
        cat nginx.conf | sed "/# UPSTREAM_PLACEHOLDER/r nginx_upstream.conf" | sed "/# UPSTREAM_PLACEHOLDER/d" > nginx_active.conf
        nginx -c $(pwd)/nginx_active.conf -g "daemon off; master_process on;" 2>&1 >/dev/null | head -10 >> nginx_proxy.log 2>&1 &
        sleep 2
        ;;
    hotpotato)
        cleanup
        start_hotpotato_workers $NUM_WORKERS
        echo "[+] Starting Hot Potato Gateway on port 8443..."
        BUILD_DIR="/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build"
        CERT="../libtlspeek/certs/server.crt"
        KEY="../libtlspeek/certs/server.key"
        $BUILD_DIR/gateway 8443 $CERT $KEY $NUM_WORKERS > gateway.log 2>&1 &
        sleep 2
        ;;
    *)
        echo "Usage: $0 start [nginx_direct|nginx_proxy|hotpotato] [num_workers]"
        echo "       $0 stop"
        echo ""
        echo "Examples:"
        echo "  $0 start nginx_direct 100     # 100 workers for direct mode"
        echo "  $0 start nginx_proxy 100      # 100 workers + nginx proxy"
        echo "  $0 start hotpotato 100        # 100 workers + gateway"
        exit 1
        ;;
esac

echo "[✓] Servers started. Check logs: *\.log"
