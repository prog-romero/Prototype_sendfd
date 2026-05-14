#!/usr/bin/env bash
# run_case1_proto.sh — Run Case 1 against the proto gateway (port 9444).
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
: "${FUNCTION:=bench2-fn-a}"
: "${OUT:=${OUT_DIR}/proto_case1_p${PAYLOAD_SIZE}.csv}"

python3 "${CLIENT}" \
    --host "${PI_HOST}" \
    --port 9444 \
    --mode case1 \
    --function "${FUNCTION}" \
    --num-requests "${NUM_REQUESTS}" \
    --payload-size "${PAYLOAD_SIZE}" \
    --warmup "${WARMUP}" \
    --requests-per-conn "${REQUESTS_PER_CONN}" \
    --out "${OUT}"

echo "[done] proto case1 → ${OUT}"
