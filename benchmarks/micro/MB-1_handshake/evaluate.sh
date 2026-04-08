#!/bin/bash

################################################################################
# COMPLETE EVALUATION - MASTER SCRIPT
# 
# This script coordinates the COMPLETE evaluation workflow:
# 1. Full project sync to Pi (keeps both machines identical)
# 2. Compile server on Pi
# 3. Compile client locally
# 4. Run full benchmark suite
# 5. Analyze and save results
#
# Usage: chmod +x evaluate.sh
#        ./evaluate.sh
################################################################################

set -e

# COLOR CODES
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Find project root (handle running from any subdirectory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
while [ ! -d "$PROJECT_ROOT/wolfssl" ] && [ "$PROJECT_ROOT" != "/" ]; do
    PROJECT_ROOT="$(dirname "$PROJECT_ROOT")"
done
if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}✗ Cannot find project root (wolfssl directory not found)${NC}"
    exit 1
fi
cd "$PROJECT_ROOT"

# CONFIGURATION - EDIT THESE
PI_USER="${1:-romero}"
PI_IP="${2:-192.168.2.2}"
PI_HOST="${PI_USER}@${PI_IP}"
TEST_SIZES="${3:-5000 6000 8000 10000}"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  COMPLETE MB-1 EVALUATION - MASTER SCRIPT                 ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Pi:            ${PI_USER}@${PI_IP}"
echo "  Test Sizes:    $TEST_SIZES"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# STEP 0: VERIFY PREREQUISITES
# ─────────────────────────────────────────────────────────────────────────────

echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 0: VERIFY PREREQUISITES${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

echo -e "${YELLOW}[1/3] Checking connectivity...${NC}"
if ssh -o ConnectTimeout=5 "$PI_HOST" "echo OK" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Pi is reachable${NC}"
else
    echo -e "${RED}✗ Cannot reach Pi at $PI_IP${NC}"
    exit 1
fi

echo -e "${YELLOW}[2/3] Checking local environment...${NC}"
if [ -d "$PROJECT_ROOT/wolfssl" ] && [ -d "$PROJECT_ROOT/benchmarks/micro/MB-1_handshake" ]; then
    echo -e "${GREEN}✓ Project structure OK (root: $PROJECT_ROOT)${NC}"
else
    echo -e "${RED}✗ Project structure invalid${NC}"
    exit 1
fi

echo -e "${YELLOW}[3/3] Checking required tools...${NC}"
for tool in gcc ssh rsync; do
    if command -v $tool &> /dev/null; then
        echo -e "  ${GREEN}✓${NC} $tool"
    else
        echo -e "  ${YELLOW}⚠${NC} $tool (optional)"
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# STEP 1: SYNC PROJECT TO PI
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 1: SYNC ENTIRE PROJECT TO PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

if [ ! -f "benchmarks/micro/MB-1_handshake/full_sync_to_pi.sh" ]; then
    echo -e "${RED}ERROR: full_sync_to_pi.sh not found${NC}"
    exit 1
fi

bash "benchmarks/micro/MB-1_handshake/full_sync_to_pi.sh" "$PI_USER" "$PI_IP" || {
    echo -e "${RED}Sync failed${NC}"
    exit 1
}

# ─────────────────────────────────────────────────────────────────────────────
# STEP 2: COMPILE ON PI
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 2: COMPILE SERVER ON PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

PIProject_NAME=$(basename "$PROJECT_ROOT")

echo "Compiling on Pi..."
ssh "$PI_HOST" << EOFPI
cd ~/${PIProject_NAME}/benchmarks/micro/MB-1_handshake
bash setup_and_compile.sh pi
EOFPI

if ssh "$PI_HOST" "test -f ~/${PIProject_Name}/benchmarks/micro/MB-1_handshake/mb1_server_pi"; then
    echo -e "${GREEN}✓ Server compiled on Pi${NC}"
else
    echo -e "${RED}✗ Server compilation failed${NC}"
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 3: COMPILE LOCALLY
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 3: COMPILE CLIENT LOCALLY${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

cd "$PROJECT_ROOT/benchmarks/micro/MB-1_handshake"
bash setup_and_compile.sh local || {
    echo -e "${RED}Local compilation failed${NC}"
    exit 1
}
cd - > /dev/null

# ─────────────────────────────────────────────────────────────────────────────
# STEP 4: INSTRUCTIONS FOR SERVER START
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 4: START SERVER ON PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

echo -e "${RED}>>> IMPORTANT: Open a NEW terminal and run:${NC}"
echo ""
echo "    ssh $PI_HOST"
echo "    cd Prototype_sendfd/benchmarks/micro/MB-1_handshake"
echo "    ./mb1_server_pi"
echo ""
echo -e "${RED}>>> Keep that terminal OPEN while we run benchmarks${NC}"
echo ""
echo -e "${YELLOW}>>> Press ENTER when server is running on Pi...${NC}"
read -r

# Verify server is running
MAX_ATTEMPTS=5
ATTEMPT=0
while [ $ATTEMPT -lt $MAX_ATTEMPTS ]; do
    if /tmp/mb1_build/mb1_client_remote "$PI_IP" A 10 > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Server is running and responding${NC}"
        break
    fi
    ((ATTEMPT++))
    if [ $ATTEMPT -lt $MAX_ATTEMPTS ]; then
        echo "  Waiting for server... ($ATTEMPT/$MAX_ATTEMPTS)"
        sleep 2
    fi
done

if [ $ATTEMPT -eq $MAX_ATTEMPTS ]; then
    echo -e "${RED}✗ Server still not responding${NC}"
    echo "  Check Pi terminal for errors"
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 5: RUN BENCHMARKS
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 5: RUN FULL BENCHMARK SUITE${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

cd "$PROJECT_ROOT/benchmarks/micro/MB-1_handshake"
bash run_benchmark.sh "$PI_IP" "$TEST_SIZES" || {
    echo -e "${RED}Benchmark failed${NC}"
    exit 1
}
cd - > /dev/null

# ─────────────────────────────────────────────────────────────────────────────
# STEP 6: FINAL SUMMARY
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ COMPLETE EVALUATION FINISHED                           ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}All steps completed successfully!${NC}"
echo ""
echo -e "${YELLOW}Results Location:${NC}"
cd "$PROJECT_ROOT/benchmarks/micro/MB-1_handshake"
LATEST_RESULT=$(ls -t results_mb1_distributed_*.csv 2>/dev/null | head -1)
if [ -n "$LATEST_RESULT" ]; then
    echo "  $LATEST_RESULT"
    echo ""
    echo -e "${YELLOW}Results Summary:${NC}"
    head -5 "$LATEST_RESULT" | tail -4
fi
cd - > /dev/null

echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Review results in CSV file"
echo "  2. Check overhead percentage (<2% expected)"
echo "  3. Verify all handshakes succeeded (failed_count = 0)"
echo "  4. Archive results: cp results_mb1_distributed_*.csv backup/"
echo ""
echo -e "${YELLOW}To stop server on Pi:${NC}"
echo "  - In Pi terminal: Press Ctrl+C"
echo "  - Or: ssh $PI_HOST 'pkill mb1_server_pi'"
echo ""
