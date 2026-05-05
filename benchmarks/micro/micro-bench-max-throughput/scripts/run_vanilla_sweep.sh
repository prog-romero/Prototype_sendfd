#!/usr/bin/env bash
# run_vanilla_sweep.sh — Full vanilla alternate sweep for cores 1..4
#
# For each core count:
#   1. Restart faasd on the Pi
#   2. Redeploy bench2-fn-a and bench2-fn-b (fresh function containers)
#   3. Wait until both functions are healthy
#   4. Re-pin gateway + function containers to the new CPU set
#   5. Run the concurrency sweep in alternate mode
#
# Usage: bash run_vanilla_sweep.sh
#
set -euo pipefail

PI_HOST="romero@192.168.2.2"
PI_PASS="tchiaze2003"
PIN_SCRIPT="/tmp/pi_pin_all.sh"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"

GATEWAY_URL_A="https://192.168.2.2:8444/function/bench2-fn-a"
GATEWAY_URL_B="https://192.168.2.2:8444/function/bench2-fn-b"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Set FN_IMAGE in the shell to override this fallback image.
DEFAULT_FN_IMAGE="ttl.sh/bench3-keepalive-vanilla-fn-20260503114431:24h"
FN_IMAGE="${FN_IMAGE:-${DEFAULT_FN_IMAGE}}"

# Seconds to wait between health-check retries after restart
HEALTH_RETRY_S=5
# Maximum seconds to wait for the server to come back up
HEALTH_TIMEOUT_S=120

# Wait for OpenFaaS API to be reachable after faasd restart.
API_TIMEOUT_S=180
API_RETRY_S=3

# ---------------------------------------------------------------------------
# Helper: wait until OpenFaaS gateway API is reachable on the Pi
# ---------------------------------------------------------------------------
wait_openfaas_api() {
    local deadline=$(( $(date +%s) + API_TIMEOUT_S ))
    echo "[api] Waiting for OpenFaaS gateway API..."
    while true; do
        local status
        status=$(ssh "${PI_HOST}" "curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${OPENFAAS_GATEWAY}/healthz || true")
        if [[ "${status}" == "200" ]] || [[ "${status}" == "401" ]] || [[ "${status}" == "403" ]]; then
            echo "[api] OpenFaaS gateway API is reachable (healthz=${status})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for OpenFaaS API at ${OPENFAAS_GATEWAY} (last healthz=${status:-unknown})."
            exit 1
        fi
        echo "[api] Not ready yet (healthz=${status:-unknown}). Retrying in ${API_RETRY_S}s..."
        sleep "${API_RETRY_S}"
    done
}

# ---------------------------------------------------------------------------
# Helper: redeploy both benchmark functions with a known image
# ---------------------------------------------------------------------------
redeploy_functions() {
    local image="$1"
    echo "[deploy] Redeploying bench2-fn-a and bench2-fn-b with image: ${image}"

    ssh "${PI_HOST}" "\
        faas-cli remove bench2-fn-a --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true; \
        faas-cli remove bench2-fn-b --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true; \
        faas-cli deploy --image ${image} --name bench2-fn-a --gateway ${OPENFAAS_GATEWAY} \
          --env BENCH2_WORKER_NAME=bench2-fn-a --env BENCH2_LISTEN_PORT=8080 \
          --label com.openfaas.scale.zero=false; \
        faas-cli deploy --image ${image} --name bench2-fn-b --gateway ${OPENFAAS_GATEWAY} \
          --env BENCH2_WORKER_NAME=bench2-fn-b --env BENCH2_LISTEN_PORT=8080 \
          --label com.openfaas.scale.zero=false \
    "
}

# ---------------------------------------------------------------------------
# Helper: wait until both functions respond with HTTP 200 (or any reply)
# ---------------------------------------------------------------------------
wait_healthy() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[health] Waiting for gateway + functions to be ready..."
    while true; do
        local ok_a ok_b
        ok_a=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_A}" 2>/dev/null || echo "000")
        ok_b=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_B}" 2>/dev/null || echo "000")
        if [[ "${ok_a}" == "200" ]] && [[ "${ok_b}" == "200" ]]; then
            echo "[health] Both functions are healthy (fn-a=${ok_a}, fn-b=${ok_b})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for healthy gateway (fn-a=${ok_a}, fn-b=${ok_b}). Aborting."
            exit 1
        fi
        echo "[health] Not ready yet (fn-a=${ok_a}, fn-b=${ok_b}). Retrying in ${HEALTH_RETRY_S}s..."
        sleep "${HEALTH_RETRY_S}"
    done
}

# Upload pin script once at the start
echo "[setup] Uploading pi_pin_all.sh to Pi..."
scp "${SCRIPT_DIR}/pi_pin_all.sh" "${PI_HOST}:${PIN_SCRIPT}"

# ---------------------------------------------------------------------------
# Main sweep loop
# ---------------------------------------------------------------------------
for CORES in 1 2 3 4; do
    echo ""
    echo "========================================================"
    echo "=== VANILLA ALTERNATE | CORES=${CORES} ==="
    echo "========================================================"

    # 1. Restart faasd
    echo "[restart] Restarting faasd on the Pi..."
    ssh "${PI_HOST}" "echo ${PI_PASS} | sudo -S systemctl restart faasd"
    echo "[restart] faasd restarted. Waiting for gateway API..."
    wait_openfaas_api

    # 2. Redeploy functions to guarantee fresh containers each core cycle.
    redeploy_functions "${FN_IMAGE}"

    # 3. Wait until both functions are healthy
    wait_healthy

    # 4. Re-pin to the new core set (PIDs changed after restart/redeploy)
    echo "[pin] Pinning gateway + function containers to ${CORES} core(s)..."
    ssh "${PI_HOST}" \
        "echo ${PI_PASS} | sudo -S env NUM_CORES=${CORES} bash ${PIN_SCRIPT}"

    # 5. Run the sweep in alternate mode (a,b,a,b,...)
    STABILIZE_BEFORE_SWEEP_S=15 \
    WRK_TARGET_MODE=alternate \
    WRK_FN_A=bench2-fn-a \
    WRK_FN_B=bench2-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        32 "${CORES}" vanilla \
        "${OUT_DIR}/vanilla_alt_${CORES}core_32kb_v2.csv"

    echo "[done] CORES=${CORES} sweep complete."
done

echo ""
echo "=== All 4 core sweeps finished. Results in: ${OUT_DIR} ==="
