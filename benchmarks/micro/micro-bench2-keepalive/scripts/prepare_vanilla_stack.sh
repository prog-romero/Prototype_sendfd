#!/usr/bin/env bash
# prepare_vanilla_stack.sh — Restore, redeploy, start, and smoke-test bench2 vanilla.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"
CLIENT_DIR="${SCRIPT_DIR}/../client"
LOCAL_PROXY_DIR="${SCRIPT_DIR}/../vanilla_proxy"

: "${PYTHON_BIN:=python3}"
: "${PI_SSH:=romero@192.168.2.2}"
: "${PI_HOST:=192.168.2.2}"
: "${PI_REPO_ROOT:=~/Prototype_sendfd}"
: "${UPSTREAM:=127.0.0.1:8080}"
: "${SMOKE_FN:=bench2-fn-a}"
: "${SMOKE_N:=8}"
: "${SMOKE_PAYLOAD:=64}"
: "${SMOKE_RETRIES:=10}"
: "${REMOTE_PROXY_LOG:=~/bench2_vanilla_proxy.log}"

password_q=""
if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
  password_q="PI_SUDO_PASSWORD=$(printf '%q' "${PI_SUDO_PASSWORD}") "
fi

echo "[prepare] restoring vanilla gateway on ${PI_SSH}"
scp "${SCRIPT_DIR}/pi_restore_vanilla_gw.sh" "${PI_SSH}:/tmp/pi_restore_vanilla_gw.sh"
ssh "${PI_SSH}" "${password_q}bash /tmp/pi_restore_vanilla_gw.sh && rm -f /tmp/pi_restore_vanilla_gw.sh"

echo "[prepare] deploying vanilla function image"
bash "${SCRIPT_DIR}/build_push_deploy_vanilla_function.sh"

echo "[prepare] syncing vanilla proxy source to ${PI_SSH}"
ssh "${PI_SSH}" "mkdir -p ${PI_REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/vanilla_proxy"
tar -C "${LOCAL_PROXY_DIR}" -cf - Makefile bench2_vanilla_proxy.c run_on_pi.sh | \
  ssh "${PI_SSH}" "tar -xf - -C ${PI_REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/vanilla_proxy"

echo "[prepare] restarting vanilla proxy on ${PI_SSH}:8444"
ssh "${PI_SSH}" "bash -lc 'pkill -f "'"'[b]ench2_vanilla_proxy --listen'"'" >/dev/null 2>&1 || true; cd ${PI_REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/vanilla_proxy && if command -v setsid >/dev/null 2>&1; then setsid -f bash ./run_on_pi.sh "${UPSTREAM}" > ${REMOTE_PROXY_LOG} 2>&1 < /dev/null; else nohup bash ./run_on_pi.sh "${UPSTREAM}" > ${REMOTE_PROXY_LOG} 2>&1 < /dev/null & disown; fi'"

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
    echo "[prepare] inspect the proxy log on the Pi: ${REMOTE_PROXY_LOG}" >&2
    exit 1
  fi

  echo "[prepare] smoke test failed on attempt ${attempt}/${SMOKE_RETRIES}; retrying ..."
  sleep 1
done

exit 1