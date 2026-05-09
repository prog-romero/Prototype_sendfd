#!/usr/bin/env bash
# run_vanilla_sweep.sh — HTTP vanilla max-throughput sweep for CloudLab
#
# Runs from the CLIENT machine.
# The SERVER machine runs faasd + the vanilla HTTP benchmark gateway.
#
# For each payload size (32KB, 512KB, 1024KB):
#   1. Restart faasd on the server (fresh containers)
#   2. Redeploy timing-fn-a and timing-fn-b
#   3. Wait until both functions are healthy on port 8082
#   4. Run the 8-step (threads, connections) sweep via wrk
#
# No CPU pinning.
#
# Usage:
#   bash scripts/run_vanilla_sweep.sh
#   SERVER_HOST=tchiaze@backend.example.com bash scripts/run_vanilla_sweep.sh
#
set -euo pipefail

# ---------------------------------------------------------------------------
# CloudLab configuration — override via environment variables
# ---------------------------------------------------------------------------
SERVER_HOST="${SERVER_HOST:-tchiaze@backend.tchiaze-304654.tchiaze-lab-1-pg0.utah.cloudlab.us}"

# Optional SSH identity/options for client -> server access.
SSH_KEY_PATH="${SSH_KEY_PATH:-}"
SSH_OPTS="${SSH_OPTS:-}"

SSH_BASE_OPTS=(-o StrictHostKeyChecking=accept-new)
if [[ -n "${SSH_KEY_PATH}" ]]; then
    SSH_BASE_OPTS+=(-i "${SSH_KEY_PATH}")
fi
if [[ -n "${SSH_OPTS}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_SSH_OPTS=(${SSH_OPTS})
    SSH_BASE_OPTS+=("${EXTRA_SSH_OPTS[@]}")
fi

ssh_remote() {
    ssh "${SSH_BASE_OPTS[@]}" "$@"
}

scp_remote() {
    scp "${SSH_BASE_OPTS[@]}" "$@"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"

# Server IP as reachable from the client (use experiment LAN IP if available)
SERVER_IP="${SERVER_IP:-$(ssh_remote "${SERVER_HOST}" "hostname -I | awk '{print \$1}'" 2>/dev/null || echo backend.tchiaze-304654.tchiaze-lab-1-pg0.utah.cloudlab.us)}"

GATEWAY_URL_A="http://${SERVER_IP}:8082/function/timing-fn-a"
GATEWAY_URL_B="http://${SERVER_IP}:8082/function/timing-fn-b"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Function images
FN_A_IMAGE="${FN_A_IMAGE:-ttl.sh/timing-fn-a-vanilla-ka:24h}"
FN_B_IMAGE="${FN_B_IMAGE:-ttl.sh/timing-fn-b-vanilla-ka:24h}"

# Payload sizes to benchmark (KB) — space-separated
PAYLOAD_KB_LIST="${PAYLOAD_KB_LIST:-32 512 1024}"

# Benchmark HTTP port (vanilla benchmark gateway maps 8082→8085 inside container)
BENCH_HTTP_PORT=8082

# Seconds between health-check retries
HEALTH_RETRY_S=5
HEALTH_TIMEOUT_S=120
# Max seconds to wait for port 8082 to become TCP-reachable after restart
PORT_OPEN_TIMEOUT_S=90

# Seconds to wait for OpenFaaS API after restart
API_TIMEOUT_S=180
API_RETRY_S=3

# ---------------------------------------------------------------------------
# Helper: wait until TCP port is open (benchmark gateway, port 8082)
# ---------------------------------------------------------------------------
wait_port_open() {
    local port="$1"
    local host="${SERVER_IP}"
    local deadline=$(( $(date +%s) + PORT_OPEN_TIMEOUT_S ))
    echo "[port] Waiting for TCP port ${host}:${port} (benchmark gateway)..."
    while true; do
        if bash -c "</dev/tcp/${host}/${port}" 2>/dev/null; then
            echo "[port] Port ${port} is open."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Port ${port} never opened on ${host} after ${PORT_OPEN_TIMEOUT_S}s."
            echo "[error] Ensure scripts/prepare_vanilla_stack.sh was run before this script."
            exit 1
        fi
        echo "[port] Port ${port} not yet open. Retrying in 3s..."
        sleep 3
    done
}

# ---------------------------------------------------------------------------
# Helper: wait until OpenFaaS gateway API is reachable on the server (port 8080)
# ---------------------------------------------------------------------------
wait_openfaas_api() {
    local deadline=$(( $(date +%s) + API_TIMEOUT_S ))
    echo "[api] Waiting for OpenFaaS gateway API (${OPENFAAS_GATEWAY}/healthz)..."
    while true; do
        local status
        status=$(ssh_remote "${SERVER_HOST}" \
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
    ssh_remote "${SERVER_HOST}" "
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
# Helper: wait until both functions respond HTTP 200 on port 8082
# ---------------------------------------------------------------------------
wait_healthy() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[health] Waiting for timing-fn-a and timing-fn-b on port 8082..."
    while true; do
        local ok_a ok_b
        # Use POST with empty body — timing functions expect POST.
        # Use || true (not || echo "000") so curl's own "000" output is not doubled.
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
# Setup: enable vanilla HTTP benchmark gateway (port 8082) in docker-compose
# ---------------------------------------------------------------------------
ENABLE_VANILLA_SCRIPT="/tmp/server_enable_vanilla_gw.sh"
echo "[setup] Uploading server_enable_vanilla_gateway.sh to server..."
scp_remote "${SCRIPT_DIR}/server_enable_vanilla_gateway.sh" "${SERVER_HOST}:${ENABLE_VANILLA_SCRIPT}"

echo "[setup] Patching docker-compose on server to enable vanilla HTTP gateway on port 8082..."
ssh_remote "${SERVER_HOST}" \
    "BENCH_GATEWAY_IMAGE='docker.io/local/faasd-gateway-bench3-ka-http:arm64' \
     bash ${ENABLE_VANILLA_SCRIPT}"

echo "[setup] Initial faasd restart after compose patch..."
ssh_remote "${SERVER_HOST}" "sudo systemctl restart faasd"
wait_openfaas_api
wait_port_open "${BENCH_HTTP_PORT}"
echo "[setup] Vanilla HTTP benchmark gateway is ready on port 8082."

# ---------------------------------------------------------------------------
# Main loop — one sweep per payload size
# ---------------------------------------------------------------------------
for PAYLOAD_KB in ${PAYLOAD_KB_LIST}; do
    echo ""
    echo "========================================================"
    echo "=== VANILLA HTTP | PAYLOAD=${PAYLOAD_KB}KB ==="
    echo "========================================================"

    echo "[restart] Restarting faasd on the server..."
    ssh_remote "${SERVER_HOST}" "sudo systemctl restart faasd"
    wait_openfaas_api

    redeploy_vanilla_functions
    wait_port_open "${BENCH_HTTP_PORT}"
    wait_healthy

    WRK_TARGET_MODE=alternate \
    WRK_FN_A=timing-fn-a \
    WRK_FN_B=timing-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        "${PAYLOAD_KB}" vanilla \
        "${OUT_DIR}/vanilla_http_${PAYLOAD_KB}kb_v1.csv" \
        "${GATEWAY_URL_A}"

    echo "[done] PAYLOAD=${PAYLOAD_KB}KB sweep complete."
done

echo ""
echo "=== All vanilla HTTP sweeps finished. Results in: ${OUT_DIR} ==="
echo "=== Server: ${SERVER_HOST} | Server IP: ${SERVER_IP} ==="
