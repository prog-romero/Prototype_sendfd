#!/usr/bin/env bash
# run_proto_sweep.sh — HTTP prototype max-throughput sweep for CloudLab
#
# Runs from the CLIENT machine.
# The SERVER machine runs faasd + the proto HTTP benchmark gateway.
#
# For each payload size (32KB, 512KB, 1024KB):
#   1. Restart faasd on the server
#   2. Redeploy timing-fn-a and timing-fn-b as proto keepalive workers
#   3. Wait until both functions are healthy on port 8083
#   4. Run the 8-step (threads, connections) sweep via wrk
#
# No CPU pinning.
#
# Pre-requisite: proto gateway already enabled on the server.
#   Run scripts/prepare_proto_stack.sh once before this script.
#
# Post: restores vanilla gateway docker-compose and restarts faasd.
#
# Usage:
#   bash scripts/run_proto_sweep.sh
#   SERVER_HOST=tchiaze@backend.example.com bash scripts/run_proto_sweep.sh
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

# Server IP as reachable from the client
SERVER_IP="${SERVER_IP:-$(ssh_remote "${SERVER_HOST}" "hostname -I | awk '{print \$1}'" 2>/dev/null || echo backend.tchiaze-304654.tchiaze-lab-1-pg0.utah.cloudlab.us)}"

GATEWAY_URL_A="http://${SERVER_IP}:8083/function/timing-fn-a"
GATEWAY_URL_B="http://${SERVER_IP}:8083/function/timing-fn-b"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Proto worker images
WORKER_A_IMAGE="${WORKER_A_IMAGE:-ttl.sh/timing-fn-a-ka-http:24h}"
WORKER_B_IMAGE="${WORKER_B_IMAGE:-ttl.sh/timing-fn-b-ka-http:24h}"

# Payload sizes to benchmark (KB) — space-separated
PAYLOAD_KB_LIST="${PAYLOAD_KB_LIST:-32 512 1024}"

# Stabilization after health check before sweep
TRANSITION_STABILIZE_AFTER_HEALTH_S="${TRANSITION_STABILIZE_AFTER_HEALTH_S:-10}"

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
    local host="${SERVER_IP}"
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
# Helper: wait until OpenFaaS gateway API is reachable on the server
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
# Helper: redeploy both proto worker functions
# ---------------------------------------------------------------------------
redeploy_proto_functions() {
    echo "[deploy] Redeploying proto workers:"
    echo "         timing-fn-a (image=${WORKER_A_IMAGE})"
    echo "         timing-fn-b (image=${WORKER_B_IMAGE})"
    ssh_remote "${SERVER_HOST}" "
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
# Main loop — one sweep per payload size
# ---------------------------------------------------------------------------
for PAYLOAD_KB in ${PAYLOAD_KB_LIST}; do
    echo ""
    echo "========================================================"
    echo "=== PROTO HTTP | PAYLOAD=${PAYLOAD_KB}KB ==="
    echo "========================================================"

    echo "[restart] Restarting faasd on the server..."
    ssh_remote "${SERVER_HOST}" "sudo systemctl restart faasd"
    wait_openfaas_api

    redeploy_proto_functions
    wait_port_open "${BENCH_HTTP_PORT}"
    wait_healthy

    if [[ "${TRANSITION_STABILIZE_AFTER_HEALTH_S}" -gt 0 ]]; then
        echo "[stabilize] Waiting ${TRANSITION_STABILIZE_AFTER_HEALTH_S}s after health..."
        sleep "${TRANSITION_STABILIZE_AFTER_HEALTH_S}"
    fi

    WRK_TARGET_MODE=alternate \
    WRK_FN_A=timing-fn-a \
    WRK_FN_B=timing-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        "${PAYLOAD_KB}" proto \
        "${OUT_DIR}/proto_http_${PAYLOAD_KB}kb_v1.csv" \
        "${GATEWAY_URL_A}"

    echo "[done] PAYLOAD=${PAYLOAD_KB}KB sweep complete."
done

# ---------------------------------------------------------------------------
# Teardown: restore vanilla gateway docker-compose and restart faasd
# ---------------------------------------------------------------------------
echo ""
echo "[teardown] Restoring vanilla gateway mode..."
ssh_remote "${SERVER_HOST}" "
    if [ -f /var/lib/faasd/docker-compose.vanilla.yaml ]; then
        sudo cp /var/lib/faasd/docker-compose.vanilla.yaml /var/lib/faasd/docker-compose.yaml
        sudo systemctl restart faasd
    else
        echo '[teardown] No vanilla backup compose found; leaving current compose as-is.'
    fi
"
echo "[teardown] Vanilla gateway restore step complete."

echo ""
echo "=== All proto HTTP sweeps finished. Results in: ${OUT_DIR} ==="
echo "=== Vanilla gateway restore step complete. ==="
echo "=== Server: ${SERVER_HOST} | Server IP: ${SERVER_IP} ==="