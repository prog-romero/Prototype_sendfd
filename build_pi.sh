#!/bin/bash
# build_pi.sh — Build the entire project natively on the Raspberry Pi (aarch64).
# Run this script DIRECTLY ON THE PI after copying the project:
#   bash ~/Prototype_sendfd/build_pi.sh
set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "═══════════════════════════════════════════════════════════════"
echo "  Pi Native Build"
echo "  Repo: $REPO"
echo "  Arch: $(uname -m)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ─── Check build tools ──────────────────────────────────────────────────────────
for tool in gcc make cmake autoreconf; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: '$tool' not found. Install with: sudo apt-get install -y build-essential cmake autoconf libtool"
        exit 1
    fi
done
echo "✓ Build tools found"
echo ""

# ─── STEP 1: Build wolfSSL ──────────────────────────────────────────────────────
echo "═══════════ STEP 1: Build wolfSSL ════════════"
cd "${REPO}/wolfssl"

if [ ! -f configure ]; then
    echo "[wolfSSL] Running autogen.sh..."
    ./autogen.sh
fi

echo "[wolfSSL] Configuring for aarch64 with required features..."
./configure \
    --enable-tls13 \
    --enable-opensslextra \
    --enable-sessionexport \
    --enable-session-ticket \
    --enable-keylog-export \
    --enable-aesgcm \
    --enable-chacha \
    --enable-hkdf \
    --enable-aescbc \
    --enable-static \
    --enable-shared \
    2>&1 | tail -5

echo "[wolfSSL] Building (this takes a few minutes on Pi)..."
make -j4 2>&1 | tail -5

# Verify the key symbols are present
echo "[wolfSSL] Verifying symbols..."
for sym in wolfSSL_set_tls13_secret_cb wolfSSL_set_quiet_shutdown wolfSSL_tls_export; do
    if nm src/.libs/libwolfssl.a 2>/dev/null | grep -q "$sym"; then
        echo "  ✓ $sym"
    elif nm src/.libs/libwolfssl.so 2>/dev/null | grep -q "$sym"; then
        echo "  ✓ $sym (shared)"
    else
        echo "  ✗ $sym — MISSING (build may fail at link time)"
    fi
done
echo "✓ wolfSSL built"
echo ""

# ─── STEP 2: Build libtlspeek ───────────────────────────────────────────────────
echo "═══════════ STEP 2: Build libtlspeek ════════════"
cd "${REPO}/libtlspeek"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
make -j4 2>&1 | tail -5
echo "✓ libtlspeek built"
echo ""

# ─── STEP 3: Build MB-3.1 benchmark ────────────────────────────────────────────
echo "═══════════ STEP 3: Build MB-3.1 Benchmark ════════════"
BENCH="${REPO}/benchmarks/macro/MB-3-request-transfer/MB-3-1-tls-migration"
cd "${BENCH}"
make clean >/dev/null 2>&1 || true
make all 2>&1 | grep -E "\[OK\]|error:"
echo "✓ Benchmark binaries built"
echo ""

echo "═══════════════════════════════════════════════════════════════"
echo "  All builds complete. You can now run:"
echo "  cd ${BENCH} && ./evaluate.sh"
echo "═══════════════════════════════════════════════════════════════"
