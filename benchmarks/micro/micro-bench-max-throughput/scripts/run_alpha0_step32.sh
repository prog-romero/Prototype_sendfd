#!/usr/bin/env bash
# run_alpha0_step32.sh — Run the bench3 alpha=0 payload ladder.
#
# Usage:
#   bash run_alpha0_step32.sh [proto|vanilla] [one|two]
#
# Topology:
#   one → both logical slots target the same deployed function
#   two → alternate between bench2-fn-a and bench2-fn-b
#
# Payload ladder:
#   32 KiB → 1024 KiB inclusive, step 32 KiB
#
# Per-payload request pattern:
#   - keepalive enabled
#   - 50 measured requests per payload size
#   - 0 warmup requests by default

set -euo pipefail

MODE="${1:-proto}"
TOPOLOGY="${2:-one}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/../results"
mkdir -p "${OUT_DIR}"

: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${ALPHA:=0}"
: "${REQUESTS_PER_CONN:=50}"
: "${NUM_REQUESTS_PER_PAYLOAD:=50}"
: "${WARMUP_PER_PAYLOAD:=0}"
: "${PAYLOAD_SIZES:=$(seq -s, 32768 32768 1048576)}"

case "${MODE}" in
    proto|vanilla)
        ;;
    *)
        echo "usage: bash run_alpha0_step32.sh [proto|vanilla] [one|two]" >&2
        exit 2
        ;;
esac

case "${TOPOLOGY}" in
    one|1|single)
        FUNCTION_B="${FUNCTION_A}"
        topology_label="one_container"
        ;;
    two|2|dual)
        topology_label="two_containers"
        ;;
    *)
        echo "usage: bash run_alpha0_step32.sh [proto|vanilla] [one|two]" >&2
        exit 2
        ;;
esac

timestamp="$(date +%Y%m%d_%H%M%S)"
: "${LABEL:=${MODE}_${topology_label}_step32kb_32_to_1024}"
: "${OUT:=${OUT_DIR}/combined_${MODE}_${topology_label}_alpha${ALPHA}_32kb_to_1024kb_rpc${REQUESTS_PER_CONN}_${timestamp}.csv}"

echo "[alpha0-step32] mode=${MODE} topology=${topology_label} fn_a=${FUNCTION_A} fn_b=${FUNCTION_B}"
echo "[alpha0-step32] requests_per_payload=${NUM_REQUESTS_PER_PAYLOAD} warmup=${WARMUP_PER_PAYLOAD} payload_sizes=${PAYLOAD_SIZES}"
echo "[alpha0-step32] output=${OUT}"

FUNCTION_A="${FUNCTION_A}" \
FUNCTION_B="${FUNCTION_B}" \
REQUESTS_PER_CONN="${REQUESTS_PER_CONN}" \
NUM_REQUESTS_PER_PAYLOAD="${NUM_REQUESTS_PER_PAYLOAD}" \
WARMUP_PER_PAYLOAD="${WARMUP_PER_PAYLOAD}" \
PAYLOAD_SIZES="${PAYLOAD_SIZES}" \
LABEL="${LABEL}" \
OUT="${OUT}" \
bash "${SCRIPT_DIR}/run_single_alpha_combined.sh" "${MODE}" "${ALPHA}"