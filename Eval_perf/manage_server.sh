#!/bin/bash

# Finalized Server Management Script (Port 8445)
# Usage: sudo ./manage_server.sh [start|stop] [mode]

ACTION=$1
MODE=$2

export LD_LIBRARY_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs:$LD_LIBRARY_PATH

# Increase File Descriptor limits for high concurrency
ulimit -n 65535

cleanup() {
    echo "[*] Cleaning up old processes..."
    # Force kill everything to avoid background ghost processes
    pkill -9 direct_tls_server
    pkill -9 proxy_worker
    pkill -9 nginx
    pkill -9 gateway
    pkill -9 worker
    rm -f *.sock /tmp/nginx.pid
    sleep 1
}

start_proxy_workers() {
    echo "[+] Starting Standalone Proxy Workers on port 8445..."
    gcc -Wall -O2 proxy_worker.c -o proxy_worker
    ./proxy_worker 0 > worker0.log 2>&1 &
    ./proxy_worker 1 > worker1.log 2>&1 &
    sleep 1
}

start_hotpotato_workers() {
    echo "[+] Starting Hot Potato Backend Workers (0 and 1)..."
    BUILD_DIR="/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build"
    CERT="../libtlspeek/certs/server.crt"
    KEY="../libtlspeek/certs/server.key"
    
    $BUILD_DIR/worker 0 $CERT $KEY > worker0.log 2>&1 &
    $BUILD_DIR/worker 1 $CERT $KEY > worker1.log 2>&1 &
    sleep 1
}

if [ "$ACTION" == "stop" ]; then
    cleanup
    echo "[*] All servers stopped."
    exit 0
fi

case "$MODE" in
    nginx_direct)
        cleanup
        echo "[+] Starting Harmonized Direct TLS Server on port 8445..."
        # Compile with prototype handler and common includes
        gcc -Wall -O2 direct_tls_server.c ../libtlspeek/worker/handler.c -o direct_tls_server \
            -I../libtlspeek/common -I../libtlspeek/worker \
            -I/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl \
            -L/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl/src/.libs \
            -lwolfssl
        ./direct_tls_server 8445 > direct_server.log 2>&1 &
        ;;
    nginx_proxy)
        cleanup
        start_proxy_workers
        echo "[+] Starting Nginx in PROXY mode on port 8445..."
        nginx -c $(pwd)/nginx.conf -g "daemon off;" > nginx_proxy.log 2>&1 &
        ;;
    hotpotato)
        cleanup
        start_hotpotato_workers
        echo "[+] Starting Hot Potato Gateway on port 8445..."
        BUILD_DIR="/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build"
        CERT="../libtlspeek/certs/server.crt"
        KEY="../libtlspeek/certs/server.key"
        $BUILD_DIR/gateway 8445 $CERT $KEY 2 > gateway.log 2>&1 &
        ;;
    *)
        echo "Usage: $0 start [nginx_direct|nginx_proxy|hotpotato]"
        echo "       $0 stop"
        exit 1
        ;;
esac
