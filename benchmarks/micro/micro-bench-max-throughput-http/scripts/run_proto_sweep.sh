#!/usr/bin/env bash
# run_proto_sweep.sh — HTTP prototype max-throughput sweep (cores 1..4)
#
# For each core count:
#   1. Restart faasd on the Pi (proto gateway comes back from docker-compose)
#   2. Redeploy timing-fn-a and timing-fn-b as proto keepalive workers
#   3. Wait until both functions are healthy on port 8083
#   4. Re-pin gateway + function containers to the requested CPU set
#   5. Run the concurrency sweep in alternate mode via wrk
#
# Pre-requisite: proto gateway already enabled on the Pi.
#   Run scripts/prepare_proto_stack.sh once before this script.
#   That script builds images, enables the proto gateway in docker-compose,
#   and deploys the workers.
#
# Post: restores vanilla gateway docker-compose and restarts faasd.
#
# Usage:
#   bash run_proto_sweep.sh
#   CORES_LIST="1 2" bash run_proto_sweep.sh   # run only selected core counts
#
set -euo pipefail

PI_HOST="romero@192.168.2.2"
PI_PASS="tchiaze2003"
PIN_SCRIPT="/tmp/pi_pin_all_http.sh"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"

GATEWAY_URL_A="http://192.168.2.2:8083/function/timing-fn-a"
GATEWAY_URL_B="http://192.168.2.2:8083/function/timing-fn-b"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Proto worker images — override via env if you have newer tags.
DEFAULT_WORKER_A_IMAGE="ttl.sh/timing-fn-a-ka-http:24h"
DEFAULT_WORKER_B_IMAGE="ttl.sh/timing-fn-b-ka-http:24h"
WORKER_A_IMAGE="${WORKER_A_IMAGE:-${DEFAULT_WORKER_A_IMAGE}}"
WORKER_B_IMAGE="${WORKER_B_IMAGE:-${DEFAULT_WORKER_B_IMAGE}}"

# Payload size to benchmark in KB
PAYLOAD_KB="${PAYLOAD_KB:-32}"

# Core counts to sweep — space-separated list.  Override to run a subset.
CORES_LIST="${CORES_LIST:-1 2 3 4}"

# Stabilization between core transitions (seconds)
TRANSITION_STABILIZE_AFTER_HEALTH_S="${TRANSITION_STABILIZE_AFTER_HEALTH_S:-10}"
TRANSITION_STABILIZE_AFTER_PIN_S="${TRANSITION_STABILIZE_AFTER_PIN_S:-5}"

# Stabilization before wrk sweep starts (seconds)
SWEEP_STABILIZE_BEFORE_SWEEP_S="${SWEEP_STABILIZE_BEFORE_SWEEP_S:-30}"

# Benchmark HTTP port for proto gateway
BENCH_HTTP_PORT=8083

# Health check parameters
HEALTH_RETRY_S=5
HEALTH_TIMEOUT_S=120
# Max seconds to wait for port 8083 to become TCP-reachable after restart
PORT_OPEN_TIMEOUT_S=60

# API readiness parameters
API_TIMEOUT_S=180
API_RETRY_S=3

# ---------------------------------------------------------------------------
# Helper: wait until TCP port is open (proto benchmark gateway, port 8083)
# ---------------------------------------------------------------------------
wait_port_open() {
    local port="$1"
    local host="192.168.2.2"
    local deadline=$(( $(date +%s) + PORT_OPEN_TIMEOUT_S ))
    echo "[port] Waiting for TCP port ${host}:${port} (proto gateway)..."
    while true; do
        if bash -c "</dev/tcp/${host}/${port}" 2>/dev/null; then
            echo "[port] Port ${port} is open."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Port ${port} never opened on ${host} after ${PORT_OPEN_TIMEOUT_S}s."
            echo "[error] Ensure scripts/prepare_proto_stack.sh was run before this script."
            exit 1
        fi
        echo "[port] Port ${port} not yet open. Retrying in 3s..."
        sleep 3
    done
}

# ---------------------------------------------------------------------------
# Helper: sleep with a progress message
# ---------------------------------------------------------------------------
stabilize_phase() {
    local label="$1"
    local seconds="$2"
    if [[ "${seconds}" -le 0 ]]; then
        return 0
    fi
    echo "[stabilize] ${label}: waiting ${seconds}s..."
    sleep "${seconds}"
    echo "[stabilize] ${label}: done."
}

# ---------------------------------------------------------------------------
# Helper: wait until OpenFaaS gateway API is reachable on the Pi
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
# Helper: redeploy both proto worker functions
# ---------------------------------------------------------------------------
redeploy_proto_functions() {
    echo "[deploy] Redeploying proto workers:"
    echo "         timing-fn-a (image=${WORKER_A_IMAGE})"
    echo "         timing-fn-b (image=${WORKER_B_IMAGE})"
    ssh "${PI_HOST}" "
        faas-cli remove timing-fn-a --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true
        faas-cli remove timing-fn-b --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true
        sleep 3

        faas-cli deploy \
            --image ${WORKER_A_IMAGE} \
            --name timing-fn-a \
            --gateway ${OPENFAAS_GATEWAY} \
            --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a \
            --env HTTPMIGRATE_KA_SOCKET_DIR=/run/tlsmigrate \
            --env HTTPMIGRATE_KA_RELAY_SOCKET=/run/tlsmigrate/relay.sock \
            --label com.openfaas.scale.zero=false

        faas-cli deploy \
            --image ${WORKER_B_IMAGE} \
            --name timing-fn-b \
            --gateway ${OPENFAAS_GATEWAY} \
            --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-b \
            --env HTTPMIGRATE_KA_SOCKET_DIR=/run/tlsmigrate \
            --env HTTPMIGRATE_KA_RELAY_SOCKET=/run/tlsmigrate/relay.sock \
            --label com.openfaas.scale.zero=false
    "
}

# ---------------------------------------------------------------------------
# Helper: wait until both proto functions respond HTTP 200 on port 8083
# ---------------------------------------------------------------------------
wait_healthy() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[health] Waiting for timing-fn-a and timing-fn-b on port 8083..."
    while true; do
        local ok_a ok_b
        # POST with empty body; || true avoids doubling the "000" code on failure.
        ok_a=$(curl -s -X POST -H "Content-Length: 0" \
            -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_A}" 2>/dev/null || true)
        ok_b=$(curl -s -X POST -H "Content-Length: 0" \
            -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_B}" 2>/dev/null || true)
        if [[ "${ok_a}" == "200" ]] && [[ "${ok_b}" == "200" ]]; then
            echo "[health] Both proto functions healthy (fn-a=${ok_a}, fn-b=${ok_b})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for healthy proto functions (fn-a=${ok_a}, fn-b=${ok_b})."
            exit 1
        fi
        echo "[health] Not ready (fn-a=${ok_a}, fn-b=${ok_b}). Retrying in ${HEALTH_RETRY_S}s..."
        sleep "${HEALTH_RETRY_S}"
    done
}

# ---------------------------------------------------------------------------
# Setup: upload pin script once
# ---------------------------------------------------------------------------
echo "[setup] Uploading pi_pin_all.sh to Pi..."
scp "${SCRIPT_DIR}/pi_pin_all.sh" "${PI_HOST}:${PIN_SCRIPT}"

# ---------------------------------------------------------------------------
# Main sweep loop
# ---------------------------------------------------------------------------
for CORES in ${CORES_LIST}; do
    echo ""
    echo "========================================================"
    echo "=== PROTO HTTP | CORES=${CORES} ==="
    echo "========================================================"

    # 1. Restart faasd — proto docker-compose.yaml is already in place,
    #    so proto gateway comes back automatically after restart.
    echo "[restart] Restarting faasd on the Pi..."
    ssh "${PI_HOST}" "echo ${PI_PASS} | sudo -S systemctl restart faasd"
    echo "[restart] faasd restarted. Waiting for gateway API..."
    wait_openfaas_api

    # 2. Redeploy proto worker functions
    redeploy_proto_functions

    # 3. Wait for both functions to be healthy on port 8083
    # 3a. First wait for the port itself to be TCP-open
    wait_port_open "${BENCH_HTTP_PORT}"
    # 3b. Then wait for functions to return 200
    wait_healthy
    stabilize_phase "post-redeploy health" "${TRANSITION_STABILIZE_AFTER_HEALTH_S}"

    # 4. Re-pin gateway + function containers to the requested core set
    echo "[pin] Pinning gateway + function containers to ${CORES} core(s)..."
    ssh "${PI_HOST}" \
        "echo ${PI_PASS} | sudo -S env NUM_CORES=${CORES} bash ${PIN_SCRIPT}"
    stabilize_phase "post-pin core=${CORES}" "${TRANSITION_STABILIZE_AFTER_PIN_S}"

    # 5. Run the sweep in alternate mode (timing-fn-a, timing-fn-b, ...)
    STABILIZE_BEFORE_SWEEP_S="${SWEEP_STABILIZE_BEFORE_SWEEP_S}" \
    WRK_TARGET_MODE=alternate \
    WRK_FN_A=timing-fn-a \
    WRK_FN_B=timing-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        "${PAYLOAD_KB}" "${CORES}" proto \
        "${OUT_DIR}/proto_http_alt_${CORES}core_${PAYLOAD_KB}kb_v1.csv"

    echo "[done] CORES=${CORES} sweep complete."
done

# ---------------------------------------------------------------------------
# Teardown: restore vanilla gateway docker-compose and restart faasd
# ---------------------------------------------------------------------------
echo ""
echo "[teardown] Restoring vanilla gateway mode..."
ssh "${PI_HOST}" "
    echo ${PI_PASS} | sudo -S cp /var/lib/faasd/docker-compose.vanilla.yaml /var/lib/faasd/docker-compose.yaml
    echo ${PI_PASS} | sudo -S systemctl restart faasd
"
echo "[teardown] Vanilla gateway restored and faasd restarted."

echo ""
echo "=== All proto HTTP core sweeps finished. Results in: ${OUT_DIR} ==="
echo "=== Vanilla gateway has been restored. ==="
