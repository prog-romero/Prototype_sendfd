#!/bin/bash

################################################################################
# FULL PROJECT SYNC TO PI
# 
# Syncs entire Prototype_sendfd directory to Pi
# Replaces old directory on Pi completely
# Both machines will have identical content
#
# Usage: chmod +x full_sync_to_pi.sh
#        ./full_sync_to_pi.sh
################################################################################

set -e

# COLOR CODES
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# CONFIGURATION
PI_USER="${1:-romero}"
PI_IP="${2:-192.168.2.2}"
PI_HOST="${PI_USER}@${PI_IP}"

# Find project root (containing Prototype_sendfd)
# Go up until we find a directory containing both wolfssl and benchmarks
PROJECT_ROOT=$(pwd)
while [ "$PROJECT_ROOT" != "/" ]; do
    if [ -d "$PROJECT_ROOT/wolfssl" ] && [ -d "$PROJECT_ROOT/benchmarks" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}ERROR: Cannot find Prototype_sendfd root directory${NC}"
    echo "Current dir: $(pwd)"
    echo "Searched up to: $PROJECT_ROOT"
    exit 1
fi

PROJECT_NAME=$(basename "$PROJECT_ROOT")

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  FULL PROJECT SYNC TO RASPBERRY PI                        ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Pi User:        $PI_USER"
echo "  Pi IP:          $PI_IP"
echo "  Project Root:   $PROJECT_ROOT"
echo "  Project Name:   $PROJECT_NAME"
echo "  Sync Target:    ~/${PROJECT_NAME}/ (on Pi)"
echo ""

# Calculate project size
PROJ_SIZE=$(du -sh "$PROJECT_ROOT" 2>/dev/null | cut -f1)
echo -e "${YELLOW}Project size: ${PROJ_SIZE}${NC}"
echo ""

# Test connectivity
echo -e "${YELLOW}[1/4] Testing connection to Pi...${NC}"
if ssh -o ConnectTimeout=5 "$PI_HOST" "echo OK" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Connected to Pi${NC}"
else
    echo -e "${RED}✗ Cannot connect to Pi${NC}"
    echo "  Command: ssh ${PI_HOST}"
    exit 1
fi

# Warn about replacing
echo ""
echo -e "${YELLOW}[2/4] WARNING: About to replace entire project on Pi${NC}"
echo "  Current content on Pi will be DELETED"
echo "  New content will be synced from: $PROJECT_ROOT"
echo ""
read -p "  Continue? (yes/no) " -r response
if [[ ! "$response" =~ ^[Yy][Ee][Ss]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo -e "${YELLOW}[3/4] Syncing project to Pi...${NC}"
echo "  This may take 5-30 minutes depending on project size"
echo ""

# Use rsync for efficient sync (only copies changed files)
# If rsync not available, fall back to scp
if command -v rsync &> /dev/null; then
    echo "  Using rsync (efficient)..."
    rsync -avz \
        --delete \
        --exclude='.git' \
        --exclude='__pycache__' \
        --exclude='*.o' \
        --exclude='CMakeFiles' \
        --exclude='build' \
        "$PROJECT_ROOT/" "${PI_HOST}:~/${PROJECT_NAME}/" || {
        echo -e "${RED}rsync failed${NC}"
        exit 1
    }
else
    echo "  Using scp (slower but safe)..."
    # Remove old directory on Pi
    ssh "$PI_HOST" "rm -rf ~/${PROJECT_NAME}" || true
    
    # Create new directory
    ssh "$PI_HOST" "mkdir -p ~/${PROJECT_NAME}"
    
    # Copy all files
    scp -r "$PROJECT_ROOT/" "${PI_HOST}:~/${PROJECT_NAME}/" || {
        echo -e "${RED}scp failed${NC}"
        exit 1
    }
fi

echo -e "${GREEN}✓ Project synced${NC}"

# Verify sync
echo ""
echo -e "${YELLOW}[4/4] Verifying sync on Pi...${NC}"

# Check key directories exist
VERIFY_DIRS=("wolfssl" "benchmarks" "Eval_perf" "faad_optimization-Romero_New")
for dir in "${VERIFY_DIRS[@]}"; do
    if ssh "$PI_HOST" "test -d ~/${PROJECT_NAME}/${dir}"; then
        echo -e "  ${GREEN}✓${NC} ~/${PROJECT_NAME}/${dir}/"
    else
        echo -e "  ${YELLOW}⚠${NC} ~/${PROJECT_NAME}/${dir}/ (optional)"
    fi
done

echo -e "${GREEN}✓ Verification complete${NC}"

echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ FULL SYNC COMPLETE                                     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Both machines now have identical content at:${NC}"
echo "  Local:  $PROJECT_ROOT"
echo "  Pi:     ~/${PROJECT_NAME}/"
echo ""
echo -e "${YELLOW}Next step: Run setup_and_compile.sh${NC}"
echo ""
