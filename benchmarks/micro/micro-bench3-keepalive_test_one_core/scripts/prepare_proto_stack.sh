#!/usr/bin/env bash
# prepare_proto_stack.sh — Rebuild, redeploy, enable, and smoke-test bench3 prototype.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${SCRIPT_DIR}/../client"

: "${PYTHON_BIN:=python3}"
: "${PI_SSH:=romero@192.168.2.2}"
: "${PI_HOST:=192.168.2.2}"
: "${SMOKE_FN:=bench2-fn-a}"
: "${SMOKE_N:=8}"
: "${SMOKE_PAYLOAD:=64}"

password_q=""
if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
    password_q="PI_SUDO_PASSWORD=$(printf '%q' "${PI_SUDO_PASSWORD}") "
fi

echo "[prepare] restoring base faasd gateway on ${PI_SSH} if needed"
scp "${SCRIPT_DIR}/pi_restore_vanilla_gw.sh" "${PI_SSH}:/tmp/pi_restore_vanilla_gw.sh"
ssh "${PI_SSH}" "${password_q}bash /tmp/pi_restore_vanilla_gw.sh && rm -f /tmp/pi_restore_vanilla_gw.sh"

echo "[prepare] deploying prototype worker image"
bash "${SCRIPT_DIR}/build_push_deploy_proto_worker.sh"

echo "[prepare] deploying and enabling prototype gateway"
ENABLE_ON_PI=1 bash "${SCRIPT_DIR}/build_deploy_proto_gw.sh"

# Wait for the proto gateway to be ready on port 9444 (test function endpoint, not /healthz)
echo "[prepare] waiting for proto gateway to be ready on port 9444..."
MAX_WAIT=60
ELAPSED=0
while [[ ${ELAPSED} -lt ${MAX_WAIT} ]]; do
    # Test actual function endpoint instead of /healthz (proto gateway doesn't have /healthz)
    if curl -sk --max-time 3 "https://${PI_HOST}:9444/function/${SMOKE_FN}" -X POST \
        --data-binary "test" >/dev/null 2>&1; then
        echo "[prepare] proto gateway is responding on port 9444"
        break
    fi
    ELAPSED=$((ELAPSED + 3))
    if [[ ${ELAPSED} -lt ${MAX_WAIT} ]]; then
        echo "[prepare] gateway not ready yet... retrying in 3s (${ELAPSED}/${MAX_WAIT}s)"
        sleep 3
    fi
done

if [[ ${ELAPSED} -ge ${MAX_WAIT} ]]; then
    echo "[error] timeout waiting for proto gateway on port 9444"
    exit 1
fi

echo "[prepare] smoke-testing https://${PI_HOST}:9444/function/${SMOKE_FN}"
"${PYTHON_BIN}" "${CLIENT_DIR}/simple_test.py" \
    --host "${PI_HOST}" \
    --port 9444 \
    --fn "${SMOKE_FN}" \
    --n "${SMOKE_N}" \
    --payload "${SMOKE_PAYLOAD}"

echo "[ok] prototype stack is rebuilt and responding"