#!/usr/bin/env bash
# prepare_proto_stack.sh — Rebuild, redeploy, enable, and smoke-test the copied prototype path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${SCRIPT_DIR}/../client"

: "${PYTHON_BIN:=python3}"
: "${PI_SSH:=romero@192.168.2.2}"
: "${PI_HOST:=192.168.2.2}"
: "${SMOKE_FN:=bench2-fn-a}"
: "${SMOKE_N:=8}"
: "${SMOKE_PAYLOAD:=64}"
: "${SMOKE_RETRIES:=12}"
: "${SMOKE_DELAY_SECONDS:=1}"

echo "[prepare] deploying prototype worker image"
bash "${SCRIPT_DIR}/build_push_deploy_proto_worker.sh"

echo "[prepare] deploying and enabling prototype gateway"
ENABLE_ON_PI=1 bash "${SCRIPT_DIR}/build_deploy_proto_gw.sh"

echo "[prepare] smoke-testing https://${PI_HOST}:9444/function/${SMOKE_FN}"
for attempt in $(seq 1 "${SMOKE_RETRIES}"); do
    if "${PYTHON_BIN}" "${CLIENT_DIR}/simple_test.py" \
        --host "${PI_HOST}" \
        --port 9444 \
        --fn "${SMOKE_FN}" \
        --n "${SMOKE_N}" \
        --payload "${SMOKE_PAYLOAD}"
    then
        echo "[ok] prototype stack is rebuilt and responding"
        exit 0
    fi

    if [[ "${attempt}" == "${SMOKE_RETRIES}" ]]; then
        echo "[prepare] prototype smoke test did not succeed after ${SMOKE_RETRIES} attempts" >&2
        echo "[prepare] check the Pi gateway state, for example:" >&2
        echo "[prepare]   ssh ${PI_SSH} 'ss -ltn | grep 9444 || true'" >&2
        echo "[prepare]   ssh ${PI_SSH} 'sudo journalctl -u faasd -n 80 --no-pager | cat'" >&2
        exit 1
    fi

    echo "[prepare] prototype smoke test failed on attempt ${attempt}/${SMOKE_RETRIES}; retrying ..."
    sleep "${SMOKE_DELAY_SECONDS}"
done

exit 1