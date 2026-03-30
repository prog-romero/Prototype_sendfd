#!/bin/bash
# test/test_pipeline.sh — End-to-end integration test for libtlspeek.
#
# Starts worker 0, worker 1, the gateway, runs curl requests,
# validates responses, and shuts everything down.
#
# Run from the libtlspeek/ root directory:
#   bash test/test_pipeline.sh
#
# Requirements:
#   - Binaries built: build/gateway, build/worker
#   - Certificates:   certs/server.crt, certs/server.key, certs/ca.crt
#   - curl with TLS support

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"

GW="$BUILD_DIR/gateway"
WK="$BUILD_DIR/worker"
CERT="$ROOT_DIR/certs/server.crt"
KEY="$ROOT_DIR/certs/server.key"
CA="$ROOT_DIR/certs/ca.crt"
PORT=8443

# ── Runtime workaround for wolfSSL library versioning ─────────────────────────
# The system symlink /usr/local/lib/libwolfssl.so.44 may point to a version
# without custom migration symbols. We force the correct one.
export LD_PRELOAD="/usr/local/lib/libwolfssl.so.44.0.1"

PASS=0
FAIL=0

# ── Colour helpers ────────────────────────────────────────────────────────────
GREEN=$'\033[0;32m'
RED=$'\033[0;31m'
RESET=$'\033[0m'

ok()   { echo "${GREEN}  PASS${RESET}: $*"; PASS=$((PASS+1)); }
fail() { echo "${RED}  FAIL${RESET}: $*"; FAIL=$((FAIL+1)); }

# ── Cleanup on exit ───────────────────────────────────────────────────────────
cleanup() {
    echo ""
    echo "=== Cleaning up ==="
    kill "$GW_PID"  2>/dev/null || true
    kill "$WK0_PID" 2>/dev/null || true
    kill "$WK1_PID" 2>/dev/null || true
    rm -f /tmp/worker_0.sock /tmp/worker_1.sock
    echo "Done."
}
trap cleanup EXIT

# ── Verify binaries and certs ─────────────────────────────────────────────────
echo "=== libtlspeek end-to-end test ==="
echo ""

for f in "$GW" "$WK" "$CERT" "$KEY" "$CA"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: missing file $f"
        echo "Build the project first: mkdir -p build && cd build && cmake .. && make"
        exit 1
    fi
done

# ── Remove stale sockets ──────────────────────────────────────────────────────
rm -f /tmp/worker_0.sock /tmp/worker_1.sock

# ── Start workers ─────────────────────────────────────────────────────────────
echo "=== Starting worker 0 ==="
"$WK" 0 "$CERT" "$KEY" 2>/tmp/worker0.log &
WK0_PID=$!
sleep 0.5

echo "=== Starting worker 1 ==="
"$WK" 1 "$CERT" "$KEY" 2>/tmp/worker1.log &
WK1_PID=$!
sleep 0.5

# Verify sockets appeared
for SOCK in /tmp/worker_0.sock /tmp/worker_1.sock; do
    if [ ! -S "$SOCK" ]; then
        echo "ERROR: socket $SOCK not created — worker may have failed to start"
        echo "Worker log:"
        cat /tmp/worker0.log /tmp/worker1.log || true
        exit 1
    fi
done

# ── Start gateway ─────────────────────────────────────────────────────────────
echo "=== Starting gateway on port $PORT ==="
"$GW" $PORT "$CERT" "$KEY" 2 2>/tmp/gateway.log &
GW_PID=$!
sleep 1

# Verify gateway is listening
if ! kill -0 "$GW_PID" 2>/dev/null; then
    echo "ERROR: gateway process died"
    cat /tmp/gateway.log
    exit 1
fi

echo ""
echo "=== Running tests (curl -k --cacert $CA) ==="
echo ""

# Helper: perform a curl request and check response
check_curl() {
    local desc="$1"
    local url="$2"
    local expected="$3"
    shift 3
    local extra_opts=("$@")

    local response
    response=$(curl -s --cacert "$CA" "${extra_opts[@]}" "$url" 2>/dev/null || true)

    if echo "$response" | grep -qF "$expected"; then
        ok "$desc → got: $response"
    else
        fail "$desc — expected substring '$expected', got: $response"
    fi
}

# Test 1: /function/hello → worker 0
check_curl \
    "GET /function/hello → worker-0" \
    "https://localhost:$PORT/function/hello" \
    "Hello from worker"

sleep 0.3

# Test 2: /function/compute → worker 1
check_curl \
    "GET /function/compute → result=42" \
    "https://localhost:$PORT/function/compute" \
    "result=42"

sleep 0.3

# Test 3: POST /function/echo → echo body
check_curl \
    "POST /function/echo body='ping'" \
    "https://localhost:$PORT/function/echo" \
    "ping" \
    -X POST -d "ping"

sleep 0.3

# Test 4: Unknown path → 404
check_curl \
    "GET /unknown → 404 Not Found" \
    "https://localhost:$PORT/unknown" \
    "Not Found"

sleep 0.3

# Test 5: Verify X-Mechanism header (confirms the peek path was used)
echo ""
echo "=== Checking X-Mechanism header ==="
HEADERS=$(curl -sI --cacert "$CA" \
    "https://localhost:$PORT/function/hello" 2>/dev/null || true)

if echo "$HEADERS" | grep -qi "X-Mechanism: tls-read-peek"; then
    ok "X-Mechanism: tls-read-peek header present"
else
    fail "X-Mechanism header missing (response headers: $HEADERS)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════"
echo "  Results: ${PASS} passed, ${FAIL} failed"
echo "═══════════════════════════════════════════════════"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "=== Gateway log (last 30 lines) ==="
    tail -30 /tmp/gateway.log
    echo ""
    echo "=== Worker 0 log (last 20 lines) ==="
    tail -20 /tmp/worker0.log
    exit 1
fi

echo ""
echo "All tests passed!"
