#!/usr/bin/env bash
# run_proto_sweep.sh — Full prototype alternate sweep for cores 1..4
#
# For each core count:
#   1. Restart faasd on the Pi
#   2. Redeploy bench2-fn-a and bench2-fn-b as proto workers (fd-passing)
#   3. Wait until both functions are healthy (port 9444)
#   4. Re-pin gateway + function containers to the new CPU set
#   5. Run the concurrency sweep in alternate mode against port 9444
#
# After all 4 sweeps, vanilla gateway mode is restored.
#
# Usage: bash run_proto_sweep.sh
#
set -euo pipefail

PI_HOST="romero@192.168.2.2"
PI_ADDR="192.168.2.2"
PI_PASS="tchiaze2003"
PIN_SCRIPT="/tmp/pi_pin_all.sh"
RESTORE_VANILLA_SCRIPT="/tmp/pi_restore_vanilla_gw.sh"

GATEWAY_IMAGE="docker.io/local/bench3-keepalive-gateway:arm64"
OPENFAAS_GATEWAY="http://127.0.0.1:8080"

# Keep the same payload as vanilla evaluation unless explicitly overridden.
PROTO_PAYLOAD_KB="${PROTO_PAYLOAD_KB:-32}"

# Core values to sweep. Override with CORES_LIST="2" (single core setting)
# or CORES_LIST="1 3" for a custom subset.
CORES_LIST="${CORES_LIST:-1 2 3 4}"

# One-time worker image to reuse across the 1..4 core sweeps.
# If empty, it is built/pushed/deployed using this benchmark's worker script.
PROTO_WORKER_IMAGE="${PROTO_WORKER_IMAGE:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/../results" && pwd)"

GATEWAY_URL_A="https://${PI_ADDR}:9444/function/bench2-fn-a"
GATEWAY_URL_B="https://${PI_ADDR}:9444/function/bench2-fn-b"

HEALTH_RETRY_S=5
HEALTH_TIMEOUT_S=180
API_TIMEOUT_S=180
API_RETRY_S=3

# Extra stabilization delays to avoid measuring during transient post-restart
# and post-pinning states. All values are in seconds and can be overridden.
TRANSITION_STABILIZE_AFTER_HEALTH_S="${TRANSITION_STABILIZE_AFTER_HEALTH_S:-20}"
TRANSITION_STABILIZE_AFTER_PIN_S="${TRANSITION_STABILIZE_AFTER_PIN_S:-25}"
SWEEP_STABILIZE_BEFORE_SWEEP_S="${SWEEP_STABILIZE_BEFORE_SWEEP_S:-30}"

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
            echo "[error] Timeout waiting for OpenFaaS API (last healthz=${status:-unknown})."
            exit 1
        fi
        echo "[api] Not ready yet (healthz=${status:-unknown}). Retrying in ${API_RETRY_S}s..."
        sleep "${API_RETRY_S}"
    done
}

# ---------------------------------------------------------------------------
# Helper: wait until proto gateway is ready on port 9444
# ---------------------------------------------------------------------------
wait_proto_gateway_ready() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[proto-gw] Waiting for proto gateway on port 9444 to be ready..."
    while true; do
        # Test actual function endpoint instead of /healthz (proto gateway doesn't have /healthz)
        if curl -sk --max-time 3 "https://${PI_ADDR}:9444/function/bench2-fn-a" -X POST \
            --data-binary "test" >/dev/null 2>&1; then
            echo "[proto-gw] Proto gateway is responding on port 9444."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for proto gateway on port 9444."
            exit 1
        fi
        echo "[proto-gw] Not ready yet. Retrying in ${HEALTH_RETRY_S}s..."
        sleep "${HEALTH_RETRY_S}"
    done
}

# ---------------------------------------------------------------------------
# Helper: build/push/deploy proto worker once.
# Captures the IMAGE_REF from script output into PROTO_WORKER_IMAGE.
# ---------------------------------------------------------------------------
prepare_proto_worker_image() {
    if [[ -n "${PROTO_WORKER_IMAGE}" ]]; then
        echo "[setup] Reusing provided PROTO_WORKER_IMAGE=${PROTO_WORKER_IMAGE}"
        return 0
    fi

    echo "[setup] Building/pushing/deploying proto worker via max-throughput flow..."
    local build_output
    build_output=$(PI_SUDO_PASSWORD="${PI_PASS}" bash "${SCRIPT_DIR}/build_push_deploy_proto_worker.sh")
    printf '%s\n' "${build_output}"

    PROTO_WORKER_IMAGE=$(printf '%s\n' "${build_output}" | sed -n 's/^IMAGE_REF=//p' | tail -n 1)
    if [[ -z "${PROTO_WORKER_IMAGE}" ]]; then
        echo "[error] Failed to parse IMAGE_REF from build_push_deploy_proto_worker.sh output."
        exit 1
    fi

    echo "[setup] Proto worker image selected: ${PROTO_WORKER_IMAGE}"
}

# ---------------------------------------------------------------------------
# Helper: build/import/enable the proto gateway.
# ---------------------------------------------------------------------------
prepare_proto_gateway() {
    echo "[setup] Building/importing/enabling proto gateway via max-throughput flow..."
    PI_SUDO_PASSWORD="${PI_PASS}" \
    IMAGE_REF="${GATEWAY_IMAGE}" \
    ENABLE_ON_PI=1 \
    bash "${SCRIPT_DIR}/build_deploy_proto_gw.sh"
}

# ---------------------------------------------------------------------------
# Helper: redeploy both functions as proto workers using the ttl.sh image
# ---------------------------------------------------------------------------
redeploy_proto_functions() {
    echo "[deploy] Redeploying bench2-fn-a and bench2-fn-b as proto workers..."
    ssh "${PI_HOST}" "\
        faas-cli remove bench2-fn-a --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true; \
        faas-cli remove bench2-fn-b --gateway ${OPENFAAS_GATEWAY} >/dev/null 2>&1 || true; \
        faas-cli deploy \
          --image ${PROTO_WORKER_IMAGE} \
          --name bench2-fn-a \
          --gateway ${OPENFAAS_GATEWAY} \
          --env BENCH2_FUNCTION_NAME=bench2-fn-a \
          --env BENCH2_SOCKET_DIR=/run/bench2 \
          --env BENCH2_RELAY_SOCKET=/run/bench2/relay.sock \
          --env BENCH2_CERT=/certs/server.crt \
          --env BENCH2_KEY=/certs/server.key \
          --label com.openfaas.scale.zero=false; \
        faas-cli deploy \
          --image ${PROTO_WORKER_IMAGE} \
          --name bench2-fn-b \
          --gateway ${OPENFAAS_GATEWAY} \
          --env BENCH2_FUNCTION_NAME=bench2-fn-b \
          --env BENCH2_SOCKET_DIR=/run/bench2 \
          --env BENCH2_RELAY_SOCKET=/run/bench2/relay.sock \
          --env BENCH2_CERT=/certs/server.crt \
          --env BENCH2_KEY=/certs/server.key \
          --label com.openfaas.scale.zero=false \
    "
}

# ---------------------------------------------------------------------------
# Helper: wait until both proto functions respond on port 9444
# ---------------------------------------------------------------------------
wait_healthy() {
    local deadline=$(( $(date +%s) + HEALTH_TIMEOUT_S ))
    echo "[health] Waiting for proto gateway + functions to be ready on port 9444..."
    while true; do
        local ok_a ok_b
        ok_a=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_A}" 2>/dev/null || echo "000")
        ok_b=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 5 "${GATEWAY_URL_B}" 2>/dev/null || echo "000")
        if [[ "${ok_a}" == "200" ]] && [[ "${ok_b}" == "200" ]]; then
            echo "[health] Both proto functions are healthy (fn-a=${ok_a}, fn-b=${ok_b})."
            return 0
        fi
        if [[ $(date +%s) -ge ${deadline} ]]; then
            echo "[error] Timeout waiting for healthy proto gateway (fn-a=${ok_a}, fn-b=${ok_b}). Aborting."
            exit 1
        fi
        echo "[health] Not ready yet (fn-a=${ok_a}, fn-b=${ok_b}). Retrying in ${HEALTH_RETRY_S}s..."
        sleep "${HEALTH_RETRY_S}"
    done
}

# ---------------------------------------------------------------------------
# Helper: sleep between transition phases so the platform can settle
# ---------------------------------------------------------------------------
stabilize_phase() {
    local label="$1"
    local seconds="$2"

    if [[ "${seconds}" =~ ^[0-9]+$ ]] && [[ "${seconds}" -gt 0 ]]; then
        echo "[stabilize] ${label}: sleeping ${seconds}s to let services settle..."
        sleep "${seconds}"
    fi
}

# ---------------------------------------------------------------------------
# Helper: preflight payload check against proto gateway
# ---------------------------------------------------------------------------
check_proto_payload() {
    local payload_kb="$1"
    local payload_file
    local err_file
    local code
    local curl_rc
    local err_msg
    local attempt
    local max_attempts=12
    local retry_s=2

    payload_file="$(mktemp)"
    err_file="$(mktemp)"
    trap 'rm -f "${payload_file}" "${err_file}"' RETURN

    head -c "$((payload_kb * 1024))" /dev/zero > "${payload_file}"

    for attempt in $(seq 1 "${max_attempts}"); do
        curl_rc=0
        code="$(curl -sk -o /dev/null -w "%{http_code}" --max-time 10 \
            -X POST "${GATEWAY_URL_A}" \
            -H "Content-Type: application/octet-stream" \
            --data-binary @"${payload_file}" 2>"${err_file}")" || curl_rc=$?
        err_msg="$(tr '\n' ' ' < "${err_file}" | sed 's/[[:space:]]\+/ /g' | sed 's/^ //;s/ $//')" || true

        if [[ ${curl_rc} -eq 0 && "${code}" == "200" ]]; then
            echo "[preflight] Proto payload ${payload_kb}KB check passed (HTTP=${code}) on attempt ${attempt}/${max_attempts}."
            return 0
        fi

        echo "[preflight] attempt ${attempt}/${max_attempts} failed (curl_rc=${curl_rc}, HTTP=${code:-000}${err_msg:+, err='${err_msg}'})."

        if [[ ${attempt} -lt ${max_attempts} ]]; then
            sleep "${retry_s}"
        fi
    done

    echo "[error] Preflight failed for proto payload ${payload_kb}KB after ${max_attempts} attempts."
    exit 1
}

# ---------------------------------------------------------------------------
# One-time setup: upload scripts and enable proto gateway mode
# ---------------------------------------------------------------------------
echo "[setup] Uploading scripts to Pi..."
scp "${SCRIPT_DIR}/pi_pin_all.sh"           "${PI_HOST}:${PIN_SCRIPT}"
scp "${SCRIPT_DIR}/pi_restore_vanilla_gw.sh" "${PI_HOST}:${RESTORE_VANILLA_SCRIPT}"

# Build/push/deploy worker once using this benchmark's deployment flow.
prepare_proto_worker_image

prepare_proto_gateway

echo "[setup] Waiting for proto gateway to listen on port 9444..."
wait_proto_gateway_ready

echo "[setup] Waiting for OpenFaaS gateway API..."
wait_openfaas_api

echo "[setup] Redeploying proto workers after gateway enable..."
redeploy_proto_functions

echo "[setup] Waiting for proto functions to become healthy..."
wait_healthy

echo "[setup] Preflight checking proto payload size: ${PROTO_PAYLOAD_KB}KB"
check_proto_payload "${PROTO_PAYLOAD_KB}"

# ---------------------------------------------------------------------------
# Main sweep loop
# ---------------------------------------------------------------------------
for CORES in ${CORES_LIST}; do
    echo ""
    echo "========================================================"
    echo "=== PROTO ALTERNATE | CORES=${CORES} ==="
    echo "========================================================"

    # 1. Restart faasd (fresh containers, fresh relay socket)
    echo "[restart] Restarting faasd on the Pi..."
    ssh "${PI_HOST}" "echo ${PI_PASS} | sudo -S systemctl restart faasd"
    echo "[restart] faasd restarted. Waiting for gateway API..."
    wait_openfaas_api

    # 2. Redeploy functions as proto workers
    redeploy_proto_functions

    # 3. Wait for both functions to be healthy on port 9444
    wait_healthy
    stabilize_phase "post-redeploy health" "${TRANSITION_STABILIZE_AFTER_HEALTH_S}"

    # Re-check payload preflight on each core transition so wrk starts only
    # after request path is confirmed healthy for the configured payload size.
    check_proto_payload "${PROTO_PAYLOAD_KB}"

    # 4. Re-pin gateway + function containers to the requested core set
    echo "[pin] Pinning gateway + function containers to ${CORES} core(s)..."
    ssh "${PI_HOST}" \
        "echo ${PI_PASS} | sudo -S env NUM_CORES=${CORES} bash ${PIN_SCRIPT}"
    stabilize_phase "post-pin core=${CORES}" "${TRANSITION_STABILIZE_AFTER_PIN_S}"

    # 5. Run the sweep in alternate mode (a,b,a,b,...) against port 9444
    STABILIZE_BEFORE_SWEEP_S="${SWEEP_STABILIZE_BEFORE_SWEEP_S}" \
    WRK_TARGET_MODE=alternate \
    WRK_FN_A=bench2-fn-a \
    WRK_FN_B=bench2-fn-b \
    python3 "${SCRIPT_DIR}/sweep_throughput.py" \
        "${PROTO_PAYLOAD_KB}" "${CORES}" proto \
        "${OUT_DIR}/proto_alt_${CORES}core_${PROTO_PAYLOAD_KB}kb_v2.csv"

    echo "[done] CORES=${CORES} sweep complete."
done

# ---------------------------------------------------------------------------
# Teardown: restore vanilla gateway mode
# ---------------------------------------------------------------------------
echo ""
echo "[teardown] Restoring vanilla gateway mode..."
ssh "${PI_HOST}" \
    "echo ${PI_PASS} | sudo -S env PI_SUDO_PASSWORD=${PI_PASS} bash ${RESTORE_VANILLA_SCRIPT}"

echo ""
echo "=== All 4 proto core sweeps finished. Results in: ${OUT_DIR} ==="
echo "=== Vanilla gateway has been restored. ==="
