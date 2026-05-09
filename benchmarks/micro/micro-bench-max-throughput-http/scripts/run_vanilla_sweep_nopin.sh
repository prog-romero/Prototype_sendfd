#!/usr/bin/env bash
# run_vanilla_sweep_nopin.sh — HTTP vanilla max-throughput sweep, NO CPU pinning.
#
# CloudLab-style: sweeps payload sizes (32KB, 512KB, 1024KB) with wrk2.
# For each payload size:
#   1. Restart faasd on the Pi (fresh containers)
#   2. Redeploy timing-fn-a and timing-fn-b
#   3. Wait until both functions are healthy on the native faasd port 8080
#   4. Run the 4-step wrk2 sweep: (1t/200c) -> (2t/400c) -> (3t/600c) -> (4t/800c)
#
# No CPU pinning at all — all 4 Pi cores are used freely.
# No patched gateway needed: benchmarks go directly to the native faasd (:8080).
#
# Pre-requisite: vanilla stack already built and pushed.
#   Run scripts/prepare_vanilla_stack.sh once to build images before this script.
#
# Usage:
#   bash scripts/run_vanilla_sweep_nopin.sh
#   PAYLOAD_KB_LIST="32" bash scripts/run_vanilla_sweep_nopin.sh   # single payload
#   FN_A_IMAGE="ttl.sh/timing-fn-a-vanilla-ka:24h" bash ...        # custom image tag
#
set -euo pipefail

PI_HOST="romero@192.168.2.2"
PI_PASS="tchiaze2003"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"

GATEWAY_URL_A="http://192.168.2.2:8080/function/timing-fn-a"
GATEWAY_URL_B="http://192.168.2.2:8080/function/timing-fn-b"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Function images — override via env if you have newer tags.
DEFAULT_FN_A_IMAGE="ttl.sh/timing-fn-a-vanilla-ka:24h"
DEFAULT_FN_B_IMAGE="ttl.sh/timing-fn-b-vanilla-ka:24h"
FN_A_IMAGE="${FN_A_IMAGE:-${DEFAULT_FN_A_IMAGE}}"
FN_B_IMAGE="${FN_B_IMAGE:-${DEFAULT_FN_B_IMAGE}}"

# Payload sizes to benchmark (KB) — space-separated.  Override to run a subset.
PAYLOAD_KB_LIST="${PAYLOAD_KB_LIST:-32 512 1024}"

# Seconds between health-check retries
HEALTH_RETRY_S=5
HEALTH_TIMEOUT_S=120

# Seconds to wait for OpenFaaS API after restart
API_TIMEOUT_S=180
API_RETRY_S=3

# ---------------------------------------------------------------------------
# Helper: wait until OpenFaaS gateway API is reachable on the Pi (port 8080)
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Helper: redeploy both vanilla benchmark functions
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Helper: wait until both functions respond HTTP 200 on native faasd (:8080)
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Main sweep loop — one sweep per payload size, no CPU pinning
# ---------------------------------------------------------------------------
for PAYLOAD_KB in ${PAYLOAD_KB_LIST}; do
    echo ""
    echo "========================================================"
    echo "=== VANILLA HTTP NO-PIN | PAYLOAD=${PAYLOAD_KB}KB ==="
    echo "========================================================"

    # 1. Restart faasd (fresh containers)
    echo "[restart] Restarting faasd on the Pi..."
    ssh "${PI_HOST}" "printf '%s\n' '${PI_PASS}' | sudo -S systemctl restart faasd"
    echo "[restart] faasd restarted. Waiting for gateway API..."
    wait_openfaas_api

    # 2. Redeploy vanilla functions
    redeploy_vanilla_functions

    # 3. Wait for both functions to respond 200 via native faasd (:8080)
    wait_healthy

    # 4. Run the no-pin wrk2 sweep (4 steps: 1t/200c → 2t/400c → 3t/600c → 4t/800c)
    #    No CPU pinning step here — all 4 Pi cores are available.
    WRK_TARGET_MODE=alternate \
    WRK_FN_A=timing-fn-a \
    WRK_FN_B=timing-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        "${PAYLOAD_KB}" vanilla \
        "${OUT_DIR}/vanilla_http_nopin_${PAYLOAD_KB}kb_v1.csv"

    echo "[done] PAYLOAD=${PAYLOAD_KB}KB sweep complete."
done

echo ""
echo "=== All vanilla HTTP no-pin sweeps finished. Results in: ${OUT_DIR} ==="
