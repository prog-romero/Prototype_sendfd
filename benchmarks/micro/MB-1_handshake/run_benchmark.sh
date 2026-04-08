#!/bin/bash

################################################################################
# RUN BENCHMARKS - EXECUTE FULL TEST SUITE
# 
# Runs on your machine
# Executes all benchmarks and saves results to CSV
# 
# Usage: ./run_benchmark.sh <PI_IP> [num_runs]
# Example: ./run_benchmark.sh 192.168.2.2
#          ./run_benchmark.sh 192.168.2.2 1000  (for quick test)
################################################################################

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
PI_IP="${1:-192.168.2.2}"
TEST_SIZES="${2:-5000 6000 8000 10000}"  # Default sizes, or accept custom

# Try to find client binary
if [ -f "/tmp/mb1_build/mb1_client_remote" ]; then
    CLIENT="/tmp/mb1_build/mb1_client_remote"
elif [ -f "./mb1_client_remote" ]; then
    CLIENT="./mb1_client_remote"
else
    CLIENT="${3:-.}"  # Use third argument or current dir
fi

# Validate arguments
if [ -z "$PI_IP" ]; then
    echo -e "${RED}ERROR: Pi IP address required${NC}"
    echo "Usage: $0 <PI_IP> [optional test sizes]"
    echo "Example: $0 192.168.2.2"
    echo "Example: $0 192.168.2.2 \"1000 2000 3000\""
    exit 1
fi

# Check if client binary exists
if [ ! -f "$CLIENT" ]; then
    echo -e "${RED}ERROR: Client binary not found: $CLIENT${NC}"
    echo "Run ./setup_local.sh first"
    exit 1
fi

# Create output file with timestamp
OUTPUT_FILE="results_mb1_distributed_$(date +%Y%m%d_%H%M%S).csv"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  MB-1 DISTRIBUTED BENCHMARK EXECUTION                     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Pi IP:            $PI_IP"
echo "  Client Binary:    $CLIENT"
echo "  Test Sizes:       $TEST_SIZES"
echo "  Output File:      $OUTPUT_FILE"
echo ""

# Test connectivity
echo -e "${YELLOW}[1/3] Testing connectivity to Pi...${NC}"
if $CLIENT "$PI_IP" A 10 > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Connected to Pi${NC}"
else
    echo -e "${RED}✗ Cannot connect to Pi at $PI_IP${NC}"
    echo "  Make sure:"
    echo "    1. Server is running on Pi: ./mb1_server_pi"
    echo "    2. Firewall allows port 19445"
    echo "    3. Pi IP address is correct: $PI_IP"
    exit 1
fi

# Write CSV header
echo ""
echo -e "${YELLOW}[2/3] Running benchmarks...${NC}"
echo "config,num_handshakes,total_time_us,avg_time_per_handshake_us,stddev_time_us,failed_count" > "$OUTPUT_FILE"

# Run benchmarks
TOTAL_TESTS=0
for config in A B; do
    for size in $TEST_SIZES; do
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
    done
done

CURRENT_TEST=0

for config in A B; do
    for size in $TEST_SIZES; do
        CURRENT_TEST=$((CURRENT_TEST + 1))
        
        echo ""
        echo -n "  [$CURRENT_TEST/$TOTAL_TESTS] Running config=$config, handshakes=$size... "
        
        # Run benchmark and capture output
        OUTPUT=$($CLIENT "$PI_IP" "$config" "$size" 2>&1)
        
        # Extract CSV line (last line of output)
        CSV_LINE=$(echo "$OUTPUT" | grep "^[AB]," | tail -1)
        
        if [ -z "$CSV_LINE" ]; then
            echo -e "${RED}FAILED${NC}"
            echo "  Output: $OUTPUT"
            continue
        fi
        
        # Append to CSV
        echo "$CSV_LINE" >> "$OUTPUT_FILE"
        
        # Parse and display result
        AVG=$(echo "$CSV_LINE" | cut -d',' -f4)
        FAILED=$(echo "$CSV_LINE" | cut -d',' -f6)
        
        if [ "$FAILED" -eq 0 ]; then
            echo -e "${GREEN}OK${NC} (${AVG} µs/handshake)"
        else
            echo -e "${YELLOW}OK${NC} (${AVG} µs/handshake, ${FAILED} failed)"
        fi
        
        sleep 1  # Brief pause between tests
    done
done

# Analysis
echo ""
echo -e "${YELLOW}[3/3] Analyzing results...${NC}"
echo ""

python3 << PYTHON_EOF
import csv

print("╔════════════════════════════════════════════════════════════╗")
print("║  MB-1 DISTRIBUTED BENCHMARK RESULTS                       ║")
print("╚════════════════════════════════════════════════════════════╝")
print()

try:
    with open('$OUTPUT_FILE', 'r') as f:
        reader = csv.DictReader(f)
        
        data = {
            'A': {},
            'B': {}
        }
        
        for row in reader:
            config = row['config'].strip()
            n = int(row['num_handshakes'])
            avg = float(row['avg_time_per_handshake_us'])
            stddev = float(row['stddev_time_us'])
            failed = int(row['failed_count'])
            
            data[config][n] = {
                'avg': avg,
                'stddev': stddev,
                'failed': failed
            }
        
        print("Handshakes  │ Vanilla (A)               │ With Keylog (B)           │ Overhead")
        print("─" * 85)
        
        for n in sorted(data['A'].keys()):
            config_a = data['A'][n]
            config_b = data['B'].get(n, None)
            
            if config_b:
                overhead = ((config_b['avg'] - config_a['avg']) / config_a['avg']) * 100
                print(f"{n:>11} │ {config_a['avg']:>6.2f} ± {config_a['stddev']:>6.2f} µs │ {config_b['avg']:>6.2f} ± {config_b['stddev']:>6.2f} µs │ {overhead:>+7.2f}%")
            else:
                print(f"{n:>11} │ {config_a['avg']:>6.2f} ± {config_a['stddev']:>6.2f} µs │ {'N/A':>23} │ {'N/A':>7}")
        
        print()
        print("ℹ  All handshakes successful" if all(d['failed'] == 0 for cfg in data.values() for d in cfg.values()) else "⚠  Some handshakes failed")
        print(f"✓  Results saved to: $OUTPUT_FILE")
        print()
        
except Exception as e:
    print(f"Error analyzing results: {e}")

PYTHON_EOF

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  ✓ BENCHMARK COMPLETE                                     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Results saved to: $OUTPUT_FILE${NC}"
echo ""
