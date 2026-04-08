#!/bin/bash

################################################################################
# MB-2 SETUP AND COMPILE - FOR BOTH PI AND LOCAL MACHINE
# 
# Compiles server (Pi) and client (local) for MB-2 benchmark
# Uses local wolfSSL from project
#
# Usage: 
#   On Pi:    bash setup_and_compile.sh pi
#   Locally:  bash setup_and_compile.sh local
################################################################################

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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
    DEPTH=$((DEPTH + 1))
done

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}ERROR: Cannot find project root with wolfssl${NC}"
    exit 1
fi

MB2_DIR="${PROJECT_ROOT}/benchmarks/micro/MB-2_read_peek"
BUILD_DIR="/tmp/mb2_build"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  SETUP AND COMPILE - MB-2 BENCHMARK                      ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Platform:       $PLATFORM"
echo "  Project Root:   $PROJECT_ROOT"
echo "  MB-2 Directory: $MB2_DIR"
echo "  Build Dir:      $BUILD_DIR"
echo ""

# Check wolfSSL
echo -e "${YELLOW}[1/3] Checking local wolfSSL...${NC}"

if [ ! -d "$PROJECT_ROOT/wolfssl" ]; then
    echo -e "${RED}✗ wolfSSL not found${NC}"
    exit 1
fi

WOLFSSL_BUILD_DIR="${PROJECT_ROOT}/wolfssl"

LIBWOLFSSL=""
if [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a" ]; then
    LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a"
    echo -e "${GREEN}✓ Found static library: libwolfssl.a${NC}"
elif [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so" ]; then
    # On Pi, check if this is the right architecture
    if [ "$PLATFORM" = "pi" ]; then
        echo -e "${YELLOW}⚠ Found .so but rebuilding for Pi architecture...${NC}"
        LIBWOLFSSL=""
    else
        LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so"
        echo -e "${GREEN}✓ Found shared library: libwolfssl.so${NC}"
    fi
else
    echo -e "${YELLOW}⚠ wolfSSL not yet built${NC}"
    LIBWOLFSSL=""
fi

# Build wolfSSL if needed
if [ -z "$LIBWOLFSSL" ]; then
    echo "  Building wolfSSL for $PLATFORM..."
    
    cd "$WOLFSSL_BUILD_DIR"
    
    # Clean old builds
    if [ "$PLATFORM" = "pi" ]; then
        make distclean > /dev/null 2>&1 || true
    fi
    
    if [ ! -f "Makefile" ]; then
        ./configure --enable-tls13 > /dev/null 2>&1 || {
            echo -e "${RED}Configure failed${NC}"
            exit 1
        }
    fi
    
    make -j4 > /dev/null 2>&1 || {
        echo -e "${RED}Build failed${NC}"
        exit 1
    }
    
    cd - > /dev/null
    
    if [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a" ]; then
        LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.a"
        echo -e "${GREEN}✓ Built: libwolfssl.a${NC}"
    elif [ -f "${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so" ]; then
        LIBWOLFSSL="${WOLFSSL_BUILD_DIR}/src/.libs/libwolfssl.so"
        echo -e "${GREEN}✓ Built: libwolfssl.so${NC}"
    else
        echo -e "${RED}Build produced no library${NC}"
        exit 1
    fi
else
    echo "  Using existing wolfSSL library"
fi

echo "  Location: $LIBWOLFSSL"

# Compiler flags
WOLFSSL_CFLAGS="-I${WOLFSSL_BUILD_DIR}"
WOLFSSL_LIBS="-L${WOLFSSL_BUILD_DIR}/src/.libs -lwolfssl"

echo ""
echo -e "${YELLOW}[2/3] Compiler Configuration${NC}"
echo "  CFLAGS: $WOLFSSL_CFLAGS"
echo "  LIBS:   $WOLFSSL_LIBS"

# Compile
echo ""
echo -e "${YELLOW}[3/3] Compiling MB-2 binaries...${NC}"

if [ "$PLATFORM" = "pi" ]; then
    echo ""
    echo -e "${GREEN}▶ Compiling SERVER (for Pi)${NC}"
    
    cd "$MB2_DIR"
    
    gcc -O2 -Wall \
        ${WOLFSSL_CFLAGS} \
        -o mb2_server_pi mb2_server_pi.c \
        ${WOLFSSL_LIBS} -lpthread -lm
    
    if [ -f mb2_server_pi ]; then
        SIZE=$(ls -lh mb2_server_pi | awk '{print $5}')
        echo -e "  ${GREEN}✓ Compiled: mb2_server_pi (${SIZE})${NC}"
    else
        echo -e "  ${RED}✗ Compilation failed${NC}"
        exit 1
    fi
    
else
    echo ""
    echo -e "${GREEN}▶ Compiling CLIENT (for local machine)${NC}"
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    gcc -O2 -Wall \
        ${WOLFSSL_CFLAGS} \
        -o mb2_client_remote "${MB2_DIR}/mb2_client_remote.c" \
        ${WOLFSSL_LIBS} -lpthread -lm
    
    if [ -f mb2_client_remote ]; then
        SIZE=$(ls -lh mb2_client_remote | awk '{print $5}')
        echo -e "  ${GREEN}✓ Compiled: mb2_client_remote (${SIZE})${NC}"
    else
        echo -e "  ${RED}✗ Compilation failed${NC}"
        exit 1
    fi
fi

echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ COMPILATION COMPLETE                                   ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

if [ "$PLATFORM" = "pi" ]; then
    echo -e "${GREEN}SERVER READY${NC}"
    echo "  Binary: $MB2_DIR/mb2_server_pi"
else
    echo -e "${GREEN}CLIENT READY${NC}"
    echo "  Binary: $BUILD_DIR/mb2_client_remote"
fi

echo ""
