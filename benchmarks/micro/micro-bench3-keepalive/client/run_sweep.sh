#!/usr/bin/env bash
# run_sweep.sh — case2 full sweep: 5 alpha × 5 payload sizes
#
# Usage:
#   # vanilla gateway TLS listener (port 8444)
#   bash run_sweep.sh vanilla 192.168.2.2 8444
#
#   # proto gateway  (port 9444, sendfd-aware gateway)
#   bash run_sweep.sh proto 192.168.2.2 9444
#
# Output goes to  results/<MODE>_case2_alpha<A>_p<P>.csv
# (one file per alpha/payload combination = 25 files total)

set -euo pipefail

MODE=${1:-vanilla}      # "vanilla" or "proto"
HOST=${2:-192.168.2.2}
PORT=${3:-8444}

# ── sweep parameters ────────────────────────────────────────────────
ALPHA_VALUES=(0 10 25 40 50)
PAYLOAD_SIZES=(64 512 2048 8192 32768)

NUM_REQUESTS=500
WARMUP=20
REQUESTS_PER_CONN=50
FUNCTION_A="bench2-fn-a"
FUNCTION_B="bench2-fn-b"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

total=$(( ${#ALPHA_VALUES[@]} * ${#PAYLOAD_SIZES[@]} ))
done_count=0

echo "══════════════════════════════════════════════════════════════"
echo "  Sweep: MODE=$MODE  HOST=$HOST:$PORT"
echo "  alpha   : ${ALPHA_VALUES[*]}"
echo "  payload : ${PAYLOAD_SIZES[*]} bytes"
echo "  requests: $NUM_REQUESTS  warmup: $WARMUP  rpc: $REQUESTS_PER_CONN"
echo "  total runs: $total"
echo "══════════════════════════════════════════════════════════════"

for ALPHA in "${ALPHA_VALUES[@]}"; do
    for PAYLOAD in "${PAYLOAD_SIZES[@]}"; do
        done_count=$(( done_count + 1 ))
        OUT="$RESULTS_DIR/${MODE}_case2_alpha${ALPHA}_p${PAYLOAD}.csv"

        echo ""
        echo "── [$done_count/$total] alpha=$ALPHA  payload=${PAYLOAD}B ──"

        python3 "$SCRIPT_DIR/run_bench2_client.py" \
            --host "$HOST" \
            --port "$PORT" \
            --mode case2 \
            --function-a "$FUNCTION_A" \
            --function-b "$FUNCTION_B" \
            --num-requests "$NUM_REQUESTS" \
            --payload-size "$PAYLOAD" \
            --alpha       "$ALPHA" \
            --warmup      "$WARMUP" \
            --requests-per-conn "$REQUESTS_PER_CONN" \
            --out         "$OUT"
    done
done

echo ""
echo "══════════════════════════════════════════════════════════════"
echo "  All $total runs complete."
echo "  Results in: $RESULTS_DIR/"
ls -1 "$RESULTS_DIR/${MODE}_case2_"*.csv 2>/dev/null | wc -l | xargs -I{} echo "  {} CSV files written."
echo "══════════════════════════════════════════════════════════════"
