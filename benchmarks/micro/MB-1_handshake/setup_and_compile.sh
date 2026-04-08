#!/bin/bash

################################################################################
# SETUP AND COMPILE - FOR BOTH PI AND LOCAL MACHINE
# 
# This script:
# 1. Detects if it's running on Pi or local machine
# 2. Finds local wolfSSL in project directory
# 3. Compiles both server (Pi) and client (local) as needed
# 4. Works with ALREADY SYNCED project
#
# Usage: 
#   On Pi:    bash setup_and_compile.sh
#   Locally:  bash setup_and_compile.sh local
################################################################################

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Determine if running on Pi or local
PLATFORM="${1:-auto}"

if [ "$PLATFORM" = "auto" ]; then
    if uname -m | grep -q "aarch64\|armv7"; then
        PLATFORM="pi"
    else
        PLATFORM="local"
    fi
fi

# Find project root
PROJECT_ROOT=$(pwd)
DEPTH=0
while [ "$PROJECT_ROOT" != "/" ] && [ $DEPTH -lt 10 ]; do
    if [ -d "$PROJECT_ROOT/wolfssl" ] && [ -d "$PROJECT_ROOT/benchmarks" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
    ((DEPTH++))
done

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}ERROR: Cannot find project root with wolfssl and benchmarks${NC}"
    exit 1
fi

MB1_DIR="${PROJECT_ROOT}/benchmarks/micro/MB-1_handshake"
BUILD_DIR="/tmp/mb1_build"

if [ ! -d "$MB1_DIR" ]; then
    echo -e "${RED}ERROR: Cannot find MB-1 directory at $MB1_DIR${NC}"
    exit 1
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  SETUP AND COMPILE - MB-1 BENCHMARK                      ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Platform:       $PLATFORM"
echo "  Project Root:   $PROJECT_ROOT"
echo "  MB-1 Directory: $MB1_DIR"
echo "  Build Dir:      $BUILD_DIR"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# CHECK WOLFSSL
# ─────────────────────────────────────────────────────────────────────────────

echo -e "${YELLOW}[1/3] Checking local wolfSSL...${NC}"

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}✗ wolfSSL not found at $PROJECT_ROOT/wolfssl${NC}"
    exit 1
fi

WOLFSSL_BUILD_DIR="${PROJECT_ROOT}/wolfssl"

# Try to find libwolfssl
LIBWOLFSSL=""
if [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a" ]; then
    LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a"
    echo -e "${GREEN}✓ Found static library: libwolfssl.a${NC}"
elif [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so" ]; then
    LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so"
    echo -e "${GREEN}✓ Found shared library: libwolfssl.so${NC}"
else
    echo -e "${YELLOW}⚠ wolfSSL not yet built${NC}"
    echo "  Building wolfSSL from source..."
    
    cd "$WOLFSSL_BUILD_DIR"
    
    # Check if already configured
    if [ ! -f "Makefile" ]; then
        ./configure --enable-tlsv13 > /dev/null 2>&1 || {
            echo -e "${RED}Configure failed${NC}"
            exit 1
        }
    fi
    
    # Build
    make -j4 > /dev/null 2>&1 || {
        echo -e "${RED}Build failed${NC}"
        exit 1
    }
    
    cd - > /dev/null
    
    if [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a" ]; then
        LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a"
        echo -e "${GREEN}✓ Built: libwolfssl.a${NC}"
    else
        echo -e "${RED}Build produced no library${NC}"
        exit 1
    fi
fi

echo "  Location: $LIBWOLFSSL"

# ─────────────────────────────────────────────────────────────────────────────
# SET COMPILER FLAGS
# ─────────────────────────────────────────────────────────────────────────────

WOLFSSL_CFLAGS="-I${WOLFSSL_BUILD_DIR}"
WOLFSSL_LIBS="-L${WOLFSSL_BUILD_DIR}/src/.libs -lwolfssl"

echo ""
echo -e "${YELLOW}[2/3] Compiler Configuration${NC}"
echo "  CFLAGS: $WOLFSSL_CFLAGS"
echo "  LIBS:   $WOLFSSL_LIBS"

# ─────────────────────────────────────────────────────────────────────────────
# COMPILE
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}[3/3] Compiling MB-1 binaries...${NC}"

if [ "$PLATFORM" = "pi" ]; then
    echo ""
    echo -e "${GREEN}▶ Compiling SERVER (for Pi)${NC}"
    
    cd "$MB1_DIR"
    
    gcc -O2 -Wall \
        ${WOLFSSL_CFLAGS} \
        -o mb1_server_pi mb1_server_pi.c \
        ${WOLFSSL_LIBS} -lpthread -lm
    
    if [ -f mb1_server_pi ]; then
        SIZE=$(ls -lh mb1_server_pi | awk '{print $5}')
        echo -e "  ${GREEN}✓ Compiled: mb1_server_pi (${SIZE})${NC}"
    else
        echo -e "  ${RED}✗ Compilation failed${NC}"
        exit 1
    fi
    
else  # local machine
    echo ""
    echo -e "${GREEN}▶ Compiling CLIENT (for local machine)${NC}"
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    gcc -O2 -Wall \
        ${WOLFSSL_CFLAGS} \
        -o mb1_client_remote "${MB1_DIR}/mb1_client_remote.c" \
        ${WOLFSSL_LIBS} -lpthread -lm
    
    if [ -f mb1_client_remote ]; then
        SIZE=$(ls -lh mb1_client_remote | awk '{print $5}')
        echo -e "  ${GREEN}✓ Compiled: mb1_client_remote (${SIZE})${NC}"
    else
        echo -e "  ${RED}✗ Compilation failed${NC}"
        exit 1
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# FINAL STATUS
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ COMPILATION COMPLETE                                   ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

if [ "$PLATFORM" = "pi" ]; then
    echo -e "${GREEN}SERVER READY${NC}"
    echo "  Binary: $MB1_DIR/mb1_server_pi"
    echo ""
    echo "  Start server with:"
    echo "    cd $MB1_DIR"
    echo "    ./mb1_server_pi"
else
    echo -e "${GREEN}CLIENT READY${NC}"
    echo "  Binary: $BUILD_DIR/mb1_client_remote"
    echo ""
    echo "  Run benchmarks with:"
    echo "    $BUILD_DIR/mb1_client_remote <PI_IP> <CONFIG> <NUM_HANDSHAKES>"
    echo "    $BUILD_DIR/mb1_client_remote 192.168.2.2 A 5000"
fi

echo ""
