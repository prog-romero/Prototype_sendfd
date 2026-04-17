#!/usr/bin/env bash
# prepare_proto_stack.sh — Rebuild, redeploy, enable, and smoke-test bench2 prototype.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${SCRIPT_DIR}/../client"

: "${PYTHON_BIN:=python3}"
: "${PI_HOST:=192.168.2.2}"
: "${SMOKE_FN:=bench2-fn-a}"
: "${SMOKE_N:=8}"
: "${SMOKE_PAYLOAD:=64}"

echo "[prepare] deploying prototype worker image"
bash "${SCRIPT_DIR}/build_push_deploy_proto_worker.sh"

echo "[prepare] deploying and enabling prototype gateway"
ENABLE_ON_PI=1 bash "${SCRIPT_DIR}/build_deploy_proto_gw.sh"

echo "[prepare] smoke-testing https://${PI_HOST}:9444/function/${SMOKE_FN}"
"${PYTHON_BIN}" "${CLIENT_DIR}/simple_test.py" \
    --host "${PI_HOST}" \
    --port 9444 \
    --fn "${SMOKE_FN}" \
    --n "${SMOKE_N}" \
    --payload "${SMOKE_PAYLOAD}"

echo "[ok] prototype stack is rebuilt and responding"