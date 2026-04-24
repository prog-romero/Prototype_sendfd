#!/usr/bin/env bash
# run_case2_vanilla.sh — Run Case 2 (two functions, alpha sweep) against vanilla.
#
# Uses valid locality checkpoints by default, including the strict same-owner
# special case alpha=100.
# Override: ALPHAS="0 20 50 100" bash run_case2_vanilla.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT="${SCRIPT_DIR}/../client/run_bench2_client.py"
OUT_DIR="${SCRIPT_DIR}/../results"
mkdir -p "${OUT_DIR}"

: "${PI_HOST:=192.168.2.2}"
: "${NUM_REQUESTS:=200}"
: "${PAYLOAD_SIZE:=512}"
: "${WARMUP:=20}"
: "${REQUESTS_PER_CONN:=50}"
: "${FN_A:=bench2-fn-a}"
: "${FN_B:=bench2-fn-b}"
: "${ALPHAS:=0 20 50 100}"

for alpha in ${ALPHAS}; do
    OUT="${OUT_DIR}/vanilla_case2_alpha${alpha}_p${PAYLOAD_SIZE}.csv"
    echo "[case2-vanilla] alpha=${alpha} ..."
    python3 "${CLIENT}" \
        --host "${PI_HOST}" \
        --port 8444 \
        --mode case2 \
        --function-a "${FN_A}" \
        --function-b "${FN_B}" \
        --num-requests "${NUM_REQUESTS}" \
        --payload-size "${PAYLOAD_SIZE}" \
        --alpha "${alpha}" \
        --warmup "${WARMUP}" \
        --requests-per-conn "${REQUESTS_PER_CONN}" \
        --out "${OUT}"
    echo "[done] → ${OUT}"
done
