#!/usr/bin/env bash
# run_vanilla_single_nopin.sh — Run exactly one vanilla no-pin configuration.
#
# This script is for debugging/reproducibility of a single (threads, connections)
# point with a chosen payload and wrk2 parameters.
#
# Usage:
#   bash scripts/run_vanilla_single_nopin.sh <payload_kb> <threads> <connections>
#
# Examples:
#   bash scripts/run_vanilla_single_nopin.sh 32 1 200
#   WRK2_RATE=500 WRK2_TIMEOUT_S=30 WRK_DURATION_S=30 \
#   bash scripts/run_vanilla_single_nopin.sh 512 2 400
#
# Optional env vars:
#   PI_HOST, PI_PASS
#   FN_A_IMAGE, FN_B_IMAGE
#   WRK2_RATE, WRK2_TIMEOUT_S, WRK_DURATION_S
#   STABILIZE_BEFORE_SWEEP_S, PAUSE_BETWEEN_NOPIN_STEPS_S
#   WRK_TARGET_MODE (same|alternate), WRK_FN_A, WRK_FN_B
#   RESTART_FAASD (1|0), REDEPLOY_FUNCTIONS (1|0)
#   OUTPUT_CSV (default auto-generated in ../results)
#
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: bash scripts/run_vanilla_single_nopin.sh <payload_kb> <threads> <connections>"
    exit 1
fi

PAYLOAD_KB="$1"
THREADS="$2"
CONNECTIONS="$3"

PI_HOST="${PI_HOST:-romero@192.168.2.2}"
PI_PASS="${PI_PASS:-tchiaze2003}"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"
GATEWAY_URL_A="http://192.168.2.2:8080/function/timing-fn-a"
GATEWAY_URL_B="http://192.168.2.2:8080/function/timing-fn-b"

DEFAULT_FN_A_IMAGE="ttl.sh/timing-fn-a-vanilla-ka:24h"
DEFAULT_FN_B_IMAGE="ttl.sh/timing-fn-b-vanilla-ka:24h"
FN_A_IMAGE="${FN_A_IMAGE:-${DEFAULT_FN_A_IMAGE}}"
FN_B_IMAGE="${FN_B_IMAGE:-${DEFAULT_FN_B_IMAGE}}"

RESTART_FAASD="${RESTART_FAASD:-1}"
REDEPLOY_FUNCTIONS="${REDEPLOY_FUNCTIONS:-1}"

WRK_TARGET_MODE="${WRK_TARGET_MODE:-alternate}"
WRK_FN_A="${WRK_FN_A:-timing-fn-a}"
WRK_FN_B="${WRK_FN_B:-timing-fn-b}"

WRK2_RATE="${WRK2_RATE:-999999}"
WRK2_TIMEOUT_S="${WRK2_TIMEOUT_S:-20}"
WRK_DURATION_S="${WRK_DURATION_S:-20}"
STABILIZE_BEFORE_SWEEP_S="${STABILIZE_BEFORE_SWEEP_S:-10}"
PAUSE_BETWEEN_NOPIN_STEPS_S="${PAUSE_BETWEEN_NOPIN_STEPS_S:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"
DEFAULT_OUTPUT_CSV="${OUT_DIR}/vanilla_http_single_${PAYLOAD_KB}kb_t${THREADS}_c${CONNECTIONS}.csv"
OUTPUT_CSV="${OUTPUT_CSV:-${DEFAULT_OUTPUT_CSV}}"

API_TIMEOUT_S=180
API_RETRY_S=3
HEALTH_TIMEOUT_S=120
HEALTH_RETRY_S=5

wait_openfaas_api() {
    local deadline=$(( $(date +%s) + API_TIMEOUT_S ))
    echo "[api] Waiting for OpenFaaS gateway API (http://127.0.0.1:8080/healthz)..."
    while true; do
        local status
        status=$(ssh "${PI_HOST}" \
            "curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${OPENFAAS_GATEWAY}/healthz || true")
        if [[ "${status}" == "200" ]] || [[ "${status}" == "401" ]] || [[ "${status}" == "403" ]]; then
            echo "[api] OpenFaaS gateway API is reachable (healthz=${status})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for OpenFaaS API (last healthz=${status:-unknown})."
            exit 1
        fi
        echo "[api] Not ready (healthz=${status:-unknown}). Retrying in ${API_RETRY_S}s..."
        sleep "${API_RETRY_S}"
    done
}

redeploy_vanilla_functions() {
    echo "[deploy] Redeploying timing-fn-a (image=${FN_A_IMAGE}) and timing-fn-b (image=${FN_B_IMAGE})"
    ssh "${PI_HOST}" "
        faas-cli remove timing-fn-a --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true
        faas-cli remove timing-fn-b --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true
        sleep 2
        faas-cli deploy \
            --image ${FN_A_IMAGE} \
            --name timing-fn-a \
            --gateway ${OPENFAAS_GATEWAY} \
            --env BENCH2_WORKER_NAME=timing-fn-a \
            --label com.openfaas.scale.zero=false
        faas-cli deploy \
            --image ${FN_B_IMAGE} \
            --name timing-fn-b \
            --gateway ${OPENFAAS_GATEWAY} \
            --env BENCH2_WORKER_NAME=timing-fn-b \
            --label com.openfaas.scale.zero=false
    "
}

wait_healthy() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[health] Waiting for timing-fn-a and timing-fn-b on port 8080..."
    while true; do
        local ok_a ok_b
        ok_a=$(curl -s -X POST -H "Content-Length: 0" \
            -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_A}" 2>/dev/null || true)
        ok_b=$(curl -s -X POST -H "Content-Length: 0" \
            -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_B}" 2>/dev/null || true)

        if [[ "${ok_a}" == "200" ]] && [[ "${ok_b}" == "200" ]]; then
            echo "[health] Both functions healthy (fn-a=${ok_a}, fn-b=${ok_b})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for healthy functions (fn-a=${ok_a}, fn-b=${ok_b})."
            exit 1
        fi
        echo "[health] Not ready (fn-a=${ok_a}, fn-b=${ok_b}). Retrying in ${HEALTH_RETRY_S}s..."
        sleep "${HEALTH_RETRY_S}"
    done
}

echo "[run] VANILLA single no-pin config: payload=${PAYLOAD_KB}KB threads=${THREADS} connections=${CONNECTIONS}"

if [[ "${RESTART_FAASD}" == "1" ]]; then
    echo "[restart] Restarting faasd on the Pi..."
    ssh "${PI_HOST}" "printf '%s\n' '${PI_PASS}' | sudo -S systemctl restart faasd"
fi

wait_openfaas_api

if [[ "${REDEPLOY_FUNCTIONS}" == "1" ]]; then
    redeploy_vanilla_functions
fi

wait_healthy

mkdir -p "${OUT_DIR}"

WRK_TARGET_MODE="${WRK_TARGET_MODE}" \
WRK_FN_A="${WRK_FN_A}" \
WRK_FN_B="${WRK_FN_B}" \
WRK2_RATE="${WRK2_RATE}" \
WRK2_TIMEOUT_S="${WRK2_TIMEOUT_S}" \
WRK_DURATION_S="${WRK_DURATION_S}" \
STABILIZE_BEFORE_SWEEP_S="${STABILIZE_BEFORE_SWEEP_S}" \
PAUSE_BETWEEN_NOPIN_STEPS_S="${PAUSE_BETWEEN_NOPIN_STEPS_S}" \
NOPIN_STEPS="${THREADS}:${CONNECTIONS}" \
python3 "${SCRIPT_DIR}/sweep_throughput.py" \
    "${PAYLOAD_KB}" vanilla "${OUTPUT_CSV}"

echo "[done] Result written to: ${OUTPUT_CSV}"
