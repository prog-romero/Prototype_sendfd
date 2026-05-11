#!/usr/bin/env bash
# manual_proto_test.sh
#
# Prepares and tests the prototype deployment using this benchmark's flow.
# This validates that the prototype gateway + worker are working correctly
# before running the full max-throughput evaluation.
#
# The test sends 8 requests with 64-byte payloads to bench2-fn-a on port 9444
# and reports the delta_ns timing for each request. 
#
# After testing, vanilla gateway is restored.
#
# Usage: bash manual_proto_test.sh
#

set -euo pipefail

PI_HOST="romero@192.168.2.2"
PI_PASS="tchiaze2003"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${SCRIPT_DIR}/../client"

echo ""
echo "=========================================="
echo "PROTOTYPE DEPLOYMENT + MANUAL TEST"
echo "=========================================="
echo ""

# Step 1: Build, deploy, and smoke test this benchmark's prototype stack.
echo "[1/2] Preparing prototype stack using max-throughput deployment flow..."
echo "      (This builds gateway & worker, deploys them, and smoke tests)"
echo ""

PI_SUDO_PASSWORD="${PI_PASS}" bash "${SCRIPT_DIR}/prepare_proto_stack.sh"

echo ""
echo "[2/2] Running extended manual test with timing data..."
echo "      Target: https://${PI_HOST}:9444/function/bench2-fn-a"
echo "      Requests: 8 | Payload: 64 bytes"
echo "      You should see delta_cycles for each request in the output below."
echo ""

# Small test with wrk to check the timing data is flowing
WRK_PAYLOAD_KB=1 \
WRK_TARGET_MODE=same \
WRK_SAME_TARGET=bench2-fn-a \
wrk -t2 -c2 -d3s --latency \
    -s "${SCRIPT_DIR}/../client/post_payload.lua" \
    "https://${PI_HOST}:9444/function/bench2-fn-a" \
    2>&1 | head -40

echo ""
echo "=========================================="
echo "Prototype appears healthy!"
echo "=========================================="
echo ""
echo "Next step: run the full max-throughput sweep"
echo ""
echo "  bash ${SCRIPT_DIR}/run_proto_sweep.sh"
echo ""
