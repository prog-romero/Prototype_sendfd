#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
MODULE_DIR="${ROOT_DIR}/caddy_bench"
BIN_PATH="${MODULE_DIR}/bin/benchcaddy"
GENERATED_DIR="${MODULE_DIR}/generated"
CONFIG_PATH="${GENERATED_DIR}/bench_caddy.json"

LISTEN_ADDR="${LISTEN_ADDR:-127.0.0.1:9446}"
BENCH_PATH="${BENCH_PATH:-/bench}"
CERT_PATH="${CERT_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.crt}"
KEY_PATH="${KEY_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.key}"

mkdir -p "${GENERATED_DIR}"

"${SCRIPT_DIR}/build_bench_caddy.sh"
LISTEN_ADDR="${LISTEN_ADDR}" BENCH_PATH="${BENCH_PATH}" CERT_PATH="${CERT_PATH}" KEY_PATH="${KEY_PATH}" \
    "${SCRIPT_DIR}/generate_caddy_config.sh" "${CONFIG_PATH}"

"${BIN_PATH}" validate --config "${CONFIG_PATH}"
exec "${BIN_PATH}" run --config "${CONFIG_PATH}"