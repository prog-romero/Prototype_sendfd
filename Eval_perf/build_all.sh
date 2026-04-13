#!/bin/bash

# COMPLETE BUILD SCRIPT - Compile all components
# Run this ONCE, then you can just run: bash run_evaluation_fixed.sh 100 5 30 "100 500 1000"

set -e  # Exit on any error

echo "════════════════════════════════════════════════════════════"
echo " BUILDING ALL EVALUATION COMPONENTS"
echo "════════════════════════════════════════════════════════════"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export LD_LIBRARY_PATH=$SCRIPT_DIR/../wolfssl/src/.libs:$LD_LIBRARY_PATH

# ─── Step 1: Build wolfSSL (if not already built) ────────────────────────────
echo "[1/5] Checking wolfSSL library..."
if [ ! -f ../wolfssl/src/.libs/libwolfssl.so ]; then
    echo "  [!] wolfSSL not found. Building..."
    cd ../wolfssl
    
    if [ ! -f "configure" ]; then
        ./autogen.sh
    fi
    
    ./configure \
        --enable-tls13 \
        --enable-aesgcm \
        --enable-chacha \
        --enable-hkdf \
        --enable-opensslextra \
        --enable-keying-material \
        --enable-debug \
        --disable-shared \
        --enable-static \
        2>&1 | tail -20
    
    make -j$(nproc)
    cd "$SCRIPT_DIR"
    echo "  ✓ wolfSSL built"
else
    echo "  ✓ wolfSSL already built"
fi

# ─── Step 2: Build libtlspeek (gateway + worker) ────────────────────────────
echo "[2/5] Building libtlspeek (gateway + worker)..."
cd ../libtlspeek

if [ ! -d "build" ]; then
    mkdir -p build
fi

cd build
cmake .. 2>&1 | tail -10
make -j$(nproc) 2>&1 | tail -20

if [ -f "gateway" ] && [ -f "worker" ]; then
    echo "  ✓ Gateway built: $(file gateway | grep -o 'ELF.*')"
    echo "  ✓ Worker built: $(file worker | grep -o 'ELF.*')"
else
    echo "  ✗ Build failed"
    exit 1
fi

cd "$SCRIPT_DIR"

# ─── Step 3: Verify certificates exist ───────────────────────────────────────
echo "[3/5] Checking TLS certificates..."
if [ ! -f ../libtlspeek/certs/server.crt ] || [ ! -f ../libtlspeek/certs/server.key ]; then
    echo "  [!] Certificates not found. Generating..."
    cd ../libtlspeek/certs
    bash generate_certs.sh >/dev/null 2>&1
    cd "$SCRIPT_DIR"
    echo "  ✓ Certificates generated"
else
    echo "  ✓ Certificates already exist"
fi

# ─── Step 4: Build direct_tls_server ─────────────────────────────────────────
echo "[4/5] Building direct_tls_server..."
gcc -Wall -O2 -std=c11 \
    direct_tls_server.c ../libtlspeek/worker/handler.c \
    -I../libtlspeek/common \
    -I../libtlspeek/worker \
    -I../wolfssl \
    -L../wolfssl/src/.libs \
    -lwolfssl \
    -pthread \
    -o direct_tls_server 2>&1 | grep -v "^$"

if [ -x "direct_tls_server" ]; then
    echo "  ✓ direct_tls_server compiled"
else
    echo "  ✗ direct_tls_server build failed"
    exit 1
fi

# ─── Step 5: Build proxy_worker ──────────────────────────────────────────────
echo "[5/5] Building proxy_worker..."
gcc -Wall -O2 -std=c11 \
    proxy_worker.c \
    -pthread \
    -o proxy_worker 2>&1 | grep -v "^$"

if [ -x "proxy_worker" ]; then
    echo "  ✓ proxy_worker compiled"
else
    echo "  ✗ proxy_worker build failed"
    exit 1
fi

# ─── Verification ───────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════"
echo " ✅ BUILD COMPLETE - ALL COMPONENTS READY"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Compiled binaries:"
echo "  ✓ $(ls -lh direct_tls_server | awk '{print $9, "(" $5 ")"}')"
echo "  ✓ $(ls -lh proxy_worker | awk '{print $9, "(" $5 ")"}')"
echo "  ✓ $(ls -lh ../libtlspeek/build/gateway | awk '{print $9, "(" $5 ")"}')"
echo "  ✓ $(ls -lh ../libtlspeek/build/worker | awk '{print $9, "(" $5 ")"}')"
echo ""
echo "Certificates:"
echo "  ✓ $(ls ../libtlspeek/certs/server.crt | xargs ls -lh | awk '{print $9, "(" $5 ")"}')"
echo "  ✓ $(ls ../libtlspeek/certs/server.key | xargs ls -lh | awk '{print $9, "(" $5 ")"}')"
echo ""
echo "════════════════════════════════════════════════════════════"
echo ""
echo "🚀 NOW RUN:"
echo ""
echo "   For quick test (5 minutes):"
echo "   $ bash simple_test.sh 10 5 50"
echo ""
echo "   For full evaluation (2-3 hours):"
echo "   $ bash run_evaluation_fixed.sh 100 5 30 \"100 500 1000\""
echo ""
echo "════════════════════════════════════════════════════════════"
