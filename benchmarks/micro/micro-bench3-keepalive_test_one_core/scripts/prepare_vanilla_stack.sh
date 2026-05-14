#!/usr/bin/env bash
# prepare_vanilla_stack.sh — Rebuild, redeploy, enable, and smoke-test bench3 vanilla.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${SCRIPT_DIR}/../client"

: "${PYTHON_BIN:=python3}"
: "${PI_SSH:=romero@192.168.2.2}"
: "${PI_HOST:=192.168.2.2}"
: "${SMOKE_FN:=bench2-fn-a}"
: "${SMOKE_N:=8}"
: "${SMOKE_PAYLOAD:=64}"
: "${SMOKE_RETRIES:=10}"

password_q=""
if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
  password_q="PI_SUDO_PASSWORD=$(printf '%q' "${PI_SUDO_PASSWORD}") "
fi

echo "[prepare] restoring vanilla gateway on ${PI_SSH}"
scp "${SCRIPT_DIR}/pi_restore_vanilla_gw.sh" "${PI_SSH}:/tmp/pi_restore_vanilla_gw.sh"
ssh "${PI_SSH}" "${password_q}bash /tmp/pi_restore_vanilla_gw.sh && rm -f /tmp/pi_restore_vanilla_gw.sh"

echo "[prepare] deploying vanilla function image"
bash "${SCRIPT_DIR}/build_push_deploy_vanilla_function.sh"

echo "[prepare] deploying and enabling vanilla gateway"
ENABLE_ON_PI=1 bash "${SCRIPT_DIR}/build_deploy_vanilla_gw.sh"

echo "[prepare] smoke-testing https://${PI_HOST}:8444/function/${SMOKE_FN}"
for attempt in $(seq 1 "${SMOKE_RETRIES}"); do
  if "${PYTHON_BIN}" "${CLIENT_DIR}/simple_test.py" \
    --host "${PI_HOST}" \
    --port 8444 \
    --fn "${SMOKE_FN}" \
    --n "${SMOKE_N}" \
    --payload "${SMOKE_PAYLOAD}"
  then
    echo "[ok] vanilla stack is rebuilt and responding"
    exit 0
  fi

  if [[ "${attempt}" == "${SMOKE_RETRIES}" ]]; then
    echo "[prepare] smoke test did not succeed after ${SMOKE_RETRIES} attempts" >&2
    exit 1
  fi

  echo "[prepare] smoke test failed on attempt ${attempt}/${SMOKE_RETRIES}; retrying ..."
  sleep 1
done

exit 1