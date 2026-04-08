#!/bin/bash

################################################################################
# MB-2 COMPLETE EVALUATION - MASTER SCRIPT
# 
# Orchestrates entire MB-2 benchmark workflow:
# 1. Full project sync to Pi
# 2. Compile server on Pi
# 3. Compile client locally
# 4. Run benchmarks
# 5. Generate visualization
#
# Usage: chmod +x evaluate.sh && ./evaluate.sh
################################################################################

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PI_USER="${1:-romero}"
PI_IP="${2:-192.168.2.2}"
PI_HOST="${PI_USER}@${PI_IP}"
TEST_SIZES="${3:-all}"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  COMPLETE MB-2 EVALUATION - MASTER SCRIPT                 ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Pi:            ${PI_USER}@${PI_IP}"
echo ""

# Find project root
PROJECT_ROOT=$(pwd)
DEPTH=0
while [ "$PROJECT_ROOT" != "/" ] && [ $DEPTH -lt 10 ]; do
    if [ -d "$PROJECT_ROOT/wolfssl" ] && [ -d "$PROJECT_ROOT/benchmarks" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
    DEPTH=$((DEPTH + 1))
done

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}✗ wolfssl directory not found${NC}"
    echo "  Run from project root directory"
    exit 1
fi

cd "$PROJECT_ROOT"

# STEP 0
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
if [ -d "$PROJECT_ROOT/wolfssl" ] && [ -d "$PROJECT_ROOT/benchmarks/micro/MB-2_read_peek" ]; then
    echo -e "${GREEN}✓ Project structure OK${NC}"
else
    echo -e "${RED}✗ Project structure invalid${NC}"
    exit 1
fi

echo -e "${YELLOW}[3/3] Checking required tools...${NC}"
for tool in gcc ssh rsync python3; do
    if command -v $tool &> /dev/null; then
        echo -e "  ${GREEN}✓${NC} $tool"
    else
        echo -e "  ${YELLOW}⚠${NC} $tool (optional)"
    fi
done

# STEP 1
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 1: SYNC ENTIRE PROJECT TO PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

cd "$PROJECT_ROOT/benchmarks/micro/MB-2_read_peek"

if [ ! -f "full_sync_to_pi.sh" ]; then
    echo -e "${RED}ERROR: full_sync_to_pi.sh not found${NC}"
    exit 1
fi

bash "full_sync_to_pi.sh" "$PI_USER" "$PI_IP" || {
    echo -e "${RED}Sync failed${NC}"
    exit 1
}

# STEP 2
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 2: COMPILE SERVER ON PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

PIProject_NAME=$(basename "$PROJECT_ROOT")

echo "Compiling on Pi..."
ssh "$PI_HOST" << EOFPI
cd \$HOME/${PIProject_NAME}/benchmarks/micro/MB-2_read_peek
bash setup_and_compile.sh pi
EOFPI

if ssh "$PI_HOST" "test -f \$HOME/${PIProject_NAME}/benchmarks/micro/MB-2_read_peek/mb2_server_pi"; then
    echo -e "${GREEN}✓ Server compiled on Pi${NC}"
else
    echo -e "${RED}✗ Server compilation failed${NC}"
    exit 1
fi

# STEP 3
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 3: COMPILE CLIENT LOCALLY${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

cd "$PROJECT_ROOT/benchmarks/micro/MB-2_read_peek"
bash setup_and_compile.sh local || {
    echo -e "${RED}Local compilation failed${NC}"
    exit 1
}

# STEP 4
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 4: START SERVER ON PI${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

echo -e "${RED}>>> IMPORTANT: Open a NEW terminal and run:${NC}"
echo ""
echo "    ssh $PI_HOST"
echo "    cd Prototype_sendfd/benchmarks/micro/MB-2_read_peek"
echo "    export LD_LIBRARY_PATH=\$HOME/Prototype_sendfd/wolfssl/src/.libs:\$LD_LIBRARY_PATH"
echo "    ./mb2_server_pi"
echo ""
echo -e "${RED}>>> Keep that terminal OPEN while we run benchmarks${NC}"
echo ""
echo -e "${YELLOW}>>> Press ENTER when server is running on Pi...${NC}"
read -r

# Verify server is running
MAX_ATTEMPTS=5
ATTEMPT=0
while [ $ATTEMPT -lt $MAX_ATTEMPTS ]; do
    if timeout 2 bash -c "echo > /dev/tcp/$PI_IP/19446" 2>/dev/null; then
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

# STEP 5
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 5: RUN BENCHMARKS${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

cd "$PROJECT_ROOT/benchmarks/micro/MB-2_read_peek"
bash run_benchmark.sh "$PI_IP" || {
    echo -e "${RED}Benchmark failed${NC}"
    exit 1
}

# STEP 6
echo ""
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}STEP 6: GENERATE VISUALIZATION${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

LATEST_RESULT=$(ls -t results_mb2_distributed_*.csv 2>/dev/null | head -1)
if [ -n "$LATEST_RESULT" ]; then
    python3 plot_mb2.py --csv "$LATEST_RESULT" --no-display
else
    echo -e "${YELLOW}⚠ No results file found${NC}"
fi

# Final summary
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ COMPLETE EVALUATION FINISHED                           ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}All steps completed successfully!${NC}"
echo ""
echo -e "${YELLOW}Results Location:${NC}"
if [ -n "$LATEST_RESULT" ]; then
    echo "  $LATEST_RESULT"
fi
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Review results in CSV file"
echo "  2. Check baseline (Config A) overhead is ~100% (peek costs one decryption)"
echo "  3. Verify overhead scales with payload size"
echo "  4. Archive results: cp results_mb2_distributed_*.csv backup/"
echo ""
echo -e "${YELLOW}To stop server on Pi:${NC}"
echo "  - In Pi terminal: Press Ctrl+C"
echo "  - Or: ssh $PI_HOST 'pkill mb2_server_pi'"
echo ""
