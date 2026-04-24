#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SERVER_DIR="${ROOT_DIR}/wolfssl_server"
CERT_PATH="${CERT_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.crt}"
KEY_PATH="${KEY_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.key}"
LISTEN_HOST="${LISTEN_HOST:-127.0.0.1}"
LISTEN_PORT="${LISTEN_PORT:-9445}"

make -C "${SERVER_DIR}" clean all

cmd=(
    "${SERVER_DIR}/wolfssl_bench_server"
    --listen-host "${LISTEN_HOST}"
    --listen-port "${LISTEN_PORT}"
    --cert "${CERT_PATH}"
    --key "${KEY_PATH}"
)

if [[ -n "${CPU_CORE:-}" ]]; then
    cmd+=(--core "${CPU_CORE}")
fi

exec "${cmd[@]}"