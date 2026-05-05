#!/usr/bin/env bash
set -euo pipefail

gateway_pid=""
proxy_pid=""

cleanup() {
    if [[ -n "${proxy_pid}" ]]; then
        kill "${proxy_pid}" >/dev/null 2>&1 || true
        wait "${proxy_pid}" 2>/dev/null || true
    fi
    if [[ -n "${gateway_pid}" ]]; then
        kill "${gateway_pid}" >/dev/null 2>&1 || true
        wait "${gateway_pid}" 2>/dev/null || true
    fi
}

on_signal() {
    cleanup
    exit 143
}

trap on_signal INT TERM

./gateway &
gateway_pid="$!"

if [[ "${BENCH2_VANILLA_ENABLE:-0}" == "1" ]]; then
    ./bench2_vanilla_proxy \
        --listen "${BENCH2_VANILLA_LISTEN:-0.0.0.0:8444}" \
        --upstream "${BENCH2_VANILLA_UPSTREAM:-127.0.0.1:8080}" \
        --cert "${BENCH2_VANILLA_CERT:-/certs/server.crt}" \
        --key "${BENCH2_VANILLA_KEY:-/certs/server.key}" &
    proxy_pid="$!"
fi

set +e
if [[ -n "${proxy_pid}" ]]; then
    wait -n "${gateway_pid}" "${proxy_pid}"
    status=$?
else
    wait "${gateway_pid}"
    status=$?
fi
set -e

cleanup
exit "${status}"