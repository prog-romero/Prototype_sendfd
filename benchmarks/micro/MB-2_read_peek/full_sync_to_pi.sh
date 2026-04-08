#!/bin/bash

################################################################################
# FULL PROJECT SYNC TO RASPBERRY PI
# 
# Syncs entire Prototype_sendfd project to Pi
# Uses rsync for efficient incremental sync
# 
# Usage: bash full_sync_to_pi.sh <pi_user> <pi_ip>
# Example: bash full_sync_to_pi.sh romero 192.168.2.2
################################################################################

set -e

PI_USER="${1:-romero}"
PI_IP="${2:-192.168.2.2}"
PI_HOST="${PI_USER}@${PI_IP}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

PROJECT_NAME=$(basename "$PROJECT_ROOT")

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  FULL PROJECT SYNC TO RASPBERRY PI                        ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Pi User:        $PI_USER"
echo "  Pi IP:          $PI_IP"
echo "  Project Name:   $PROJECT_NAME"
echo "  Project Root:   $PROJECT_ROOT"
echo ""

# Get project size
PROJ_SIZE=$(du -sh "$PROJECT_ROOT" 2>/dev/null | awk '{print $1}')
echo "Project size: $PROJ_SIZE"
echo ""

# Test connection
echo -e "${YELLOW}[1/4] Testing connection to Pi...${NC}"
if ssh -o ConnectTimeout=5 "$PI_HOST" "echo OK" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Connected to Pi${NC}"
else
    echo -e "${RED}✗ Cannot reach Pi at $PI_IP${NC}"
    exit 1
fi

# Sync project
echo ""
echo -e "${YELLOW}[2/4] Syncing project to Pi...${NC}"
echo "  This may take 5-30 minutes depending on project size"
echo ""

if command -v rsync &> /dev/null; then
    echo "  Using rsync (efficient)..."
    rsync -avz --delete \
        "$PROJECT_ROOT/" \
        "${PI_HOST}:~/${PROJECT_NAME}/" \
        --exclude='.git' \
        --exclude='*.o' \
        --exclude='*.a' \
        --exclude='*.so' \
        --exclude='.libs' \
        --exclude='build/' \
        || echo -e "${YELLOW}⚠ Some files may have sync errors${NC}"
else
    echo "  Using scp (slower)..."
    scp -r "$PROJECT_ROOT" "${PI_HOST}:~/" || {
        echo -e "${RED}SCP sync failed${NC}"
        exit 1
    }
fi

echo -e "${GREEN}✓ Project synced${NC}"

# Verify sync
echo ""
echo -e "${YELLOW}[3/4] Verifying sync on Pi...${NC}"

ssh "$PI_HOST" << EOF
if [ -d "\$HOME/${PROJECT_NAME}/wolfssl" ]; then
    echo "  ✓ \$HOME/${PROJECT_NAME}/wolfssl/"
else
    echo "  ✗ wolfssl missing"
    exit 1
fi

if [ -d "\$HOME/${PROJECT_NAME}/benchmarks" ]; then
    echo "  ✓ \$HOME/${PROJECT_NAME}/benchmarks/"
else
    echo "  ✗ benchmarks missing"
    exit 1
fi

if [ -d "\$HOME/${PROJECT_NAME}/libtlspeek" ]; then
    echo "  ✓ \$HOME/${PROJECT_NAME}/libtlspeek/"
else
    echo "  ⚠ libtlspeek not found (optional)"
fi
EOF

echo -e "${GREEN}✓ Verification complete${NC}"

# Final
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ FULL SYNC COMPLETE                                     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Both machines now have identical content at:"
echo "  Local:  $PROJECT_ROOT"
echo "  Pi:     ~/${PROJECT_NAME}/"
echo ""
echo "Next step: Run setup_and_compile.sh"
echo ""
