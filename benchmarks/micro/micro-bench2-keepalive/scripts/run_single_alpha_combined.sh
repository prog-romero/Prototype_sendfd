#!/usr/bin/env bash
# run_single_alpha_combined.sh — Run the combined payload sweep for one alpha.

set -euo pipefail

MODE="${1:-proto}"
ALPHA="${2:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT="${SCRIPT_DIR}/../client/run_combined_sweep.py"
OUT_DIR="${SCRIPT_DIR}/../results"
mkdir -p "${OUT_DIR}"

: "${PYTHON_BIN:=python3}"
: "${PI_HOST:=192.168.2.2}"
: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${REQUESTS_PER_CONN:=50}"
: "${NUM_REQUESTS_PER_PAYLOAD:=}"
: "${WARMUP_PER_PAYLOAD:=}"
: "${TIMEOUT:=30}"
: "${SEED:=42}"

timestamp="$(date +%Y%m%d_%H%M%S)"

case "${MODE}" in
    proto)
        PORT="${PORT:-9444}"
        LABEL="${LABEL:-proto}"
        ;;
    vanilla)
        PORT="${PORT:-8444}"
        LABEL="${LABEL:-vanilla}"
        ;;
    *)
        echo "usage: bash run_single_alpha_combined.sh [proto|vanilla] [alpha]" >&2
        exit 2
        ;;
esac

schedule_tag=""
if [[ -n "${NUM_REQUESTS_PER_PAYLOAD}" ]]; then
    schedule_tag="_npp${NUM_REQUESTS_PER_PAYLOAD}"
fi

: "${OUT:=${OUT_DIR}/combined_${LABEL}_alpha${ALPHA}_rpc${REQUESTS_PER_CONN}${schedule_tag}_${timestamp}.csv}"

echo "[single-alpha] mode=${MODE} alpha=${ALPHA} port=${PORT} rpc=${REQUESTS_PER_CONN}"
if [[ -n "${NUM_REQUESTS_PER_PAYLOAD}" ]]; then
    echo "[single-alpha] uniform_requests_per_payload=${NUM_REQUESTS_PER_PAYLOAD} warmup_per_payload=${WARMUP_PER_PAYLOAD:-auto}"
fi
echo "[single-alpha] output=${OUT}"
args=(
    --host "${PI_HOST}"
    --port "${PORT}"
    --function-a "${FUNCTION_A}"
    --function-b "${FUNCTION_B}"
    --requests-per-conn "${REQUESTS_PER_CONN}"
    --timeout "${TIMEOUT}"
    --seed "${SEED}"
    --label "${LABEL}"
    --alpha-list "${ALPHA}"
    --out "${OUT}"
)

if [[ -n "${NUM_REQUESTS_PER_PAYLOAD}" ]]; then
    args+=(--num-requests-per-payload "${NUM_REQUESTS_PER_PAYLOAD}")
fi
if [[ -n "${WARMUP_PER_PAYLOAD}" ]]; then
    args+=(--warmup-per-payload "${WARMUP_PER_PAYLOAD}")
fi

"${PYTHON_BIN}" "${CLIENT}" "${args[@]}"

echo "[done] ${OUT}"