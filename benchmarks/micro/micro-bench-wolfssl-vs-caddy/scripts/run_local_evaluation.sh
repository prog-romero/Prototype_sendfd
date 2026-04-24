#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
CLIENT_SCRIPT="${ROOT_DIR}/client/run_request_sweep.py"
SUMMARY_SCRIPT="${SCRIPT_DIR}/summarize_results.py"
RESULTS_DIR="${ROOT_DIR}/results"
PYTHON_BIN="${PYTHON:-python3}"

HOST="${HOST:-127.0.0.1}"
WOLFSSL_PORT_EXPLICIT=0
if [[ -n "${WOLFSSL_PORT+x}" ]]; then
    WOLFSSL_PORT_EXPLICIT=1
fi
CADDY_PORT_EXPLICIT=0
if [[ -n "${CADDY_PORT+x}" ]]; then
    CADDY_PORT_EXPLICIT=1
fi
WOLFSSL_PORT="${WOLFSSL_PORT:-9445}"
CADDY_PORT="${CADDY_PORT:-9446}"
BENCH_PATH="${BENCH_PATH:-/bench}"
SIZES="${SIZES:-64,256,1024,4096,16384,65536,131072,262144,524288,1048576}"
SAMPLES="${SAMPLES:-100}"
TIMEOUT="${TIMEOUT:-10}"
POST_HANDSHAKE_DELAY_MS="${POST_HANDSHAKE_DELAY_MS:-5}"
TLS_VERSION="${TLS_VERSION:-1.2}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
RAW_OUT="${RESULTS_DIR}/wolfssl_vs_caddy_raw_${TIMESTAMP}.csv"
SUMMARY_OUT="${RESULTS_DIR}/wolfssl_vs_caddy_summary_${TIMESTAMP}.csv"
LOG_DIR="${RESULTS_DIR}/logs_${TIMESTAMP}"

mkdir -p "${RESULTS_DIR}" "${LOG_DIR}"

if [[ -x /usr/local/go/bin/go && ":${PATH}:" != *":/usr/local/go/bin:"* ]]; then
    export PATH="/usr/local/go/bin:${PATH}"
fi

port_is_listening() {
    local port="$1"

    ss -ltn "( sport = :${port} )" | grep -q ":${port}"
}

pick_available_port() {
    local port="$1"
    local avoid_port="${2:-}"

    while port_is_listening "${port}" || [[ -n "${avoid_port}" && "${port}" == "${avoid_port}" ]]; do
        port=$((port + 1))
    done

    printf '%s\n' "${port}"
}

ensure_port_available() {
    local port="$1"
    local name="$2"

    if port_is_listening "${port}"; then
        echo "ERROR: ${name}=${port} is already in use. Stop the stale listener or choose another port." >&2
        return 1
    fi

    return 0
}

resolve_ports() {
    local requested_wolfssl_port="${WOLFSSL_PORT}"
    local requested_caddy_port="${CADDY_PORT}"
    local selected_port

    if (( WOLFSSL_PORT_EXPLICIT )); then
        ensure_port_available "${WOLFSSL_PORT}" "WOLFSSL_PORT"
    else
        selected_port="$(pick_available_port "${WOLFSSL_PORT}" "${requested_caddy_port}")"
        if [[ "${selected_port}" != "${WOLFSSL_PORT}" ]]; then
            echo "[info] WOLFSSL_PORT ${WOLFSSL_PORT} is busy; using ${selected_port}" >&2
            WOLFSSL_PORT="${selected_port}"
        fi
    fi

    if (( CADDY_PORT_EXPLICIT )); then
        if [[ "${CADDY_PORT}" == "${WOLFSSL_PORT}" ]]; then
            echo "ERROR: CADDY_PORT and WOLFSSL_PORT must be different." >&2
            return 1
        fi
        ensure_port_available "${CADDY_PORT}" "CADDY_PORT"
    else
        selected_port="$(pick_available_port "${CADDY_PORT}" "${WOLFSSL_PORT}")"
        if [[ "${selected_port}" != "${CADDY_PORT}" ]]; then
            echo "[info] CADDY_PORT ${CADDY_PORT} is busy; using ${selected_port}" >&2
            CADDY_PORT="${selected_port}"
        fi
    fi

    return 0
}

wait_for_port() {
    local host="$1"
    local port="$2"
    local pid="${3:-}"
    local log_file="${4:-}"
    local attempts=100

    while (( attempts > 0 )); do
        if port_is_listening "${port}"; then
            return 0
        fi
        if [[ -n "${pid}" ]] && ! kill -0 "${pid}" >/dev/null 2>&1; then
            echo "ERROR: process ${pid} exited before ${host}:${port} started listening." >&2
            if [[ -n "${log_file}" ]]; then
                echo "See log: ${log_file}" >&2
            fi
            return 1
        fi
        attempts=$((attempts - 1))
        "${PYTHON_BIN}" - <<'PY'
import time
time.sleep(0.1)
PY
    done

    echo "ERROR: timed out waiting for ${host}:${port}" >&2
    return 1
}

cleanup_pid() {
    local pid="${1:-}"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
        kill "${pid}" >/dev/null 2>&1 || true
        wait "${pid}" >/dev/null 2>&1 || true
    fi
}

WOLFSSL_PID=""
CADDY_PID=""
trap 'cleanup_pid "${WOLFSSL_PID}"; cleanup_pid "${CADDY_PID}"' EXIT

resolve_ports

echo "[1/6] Building wolfSSL server"
make -C "${ROOT_DIR}/wolfssl_server" clean all >/dev/null

echo "[2/6] Building custom Caddy benchmark binary"
"${SCRIPT_DIR}/build_bench_caddy.sh" >/dev/null

echo "[3/6] Running wolfSSL sweep"
LISTEN_HOST="${HOST}" LISTEN_PORT="${WOLFSSL_PORT}" \
    "${SCRIPT_DIR}/run_wolfssl_server.sh" \
    >"${LOG_DIR}/wolfssl_server.log" 2>&1 &
WOLFSSL_PID="$!"
wait_for_port "${HOST}" "${WOLFSSL_PORT}" "${WOLFSSL_PID}" "${LOG_DIR}/wolfssl_server.log"
"${PYTHON_BIN}" "${CLIENT_SCRIPT}" \
    --implementation wolfssl \
    --host "${HOST}" \
    --port "${WOLFSSL_PORT}" \
    --path "${BENCH_PATH}" \
    --sizes "${SIZES}" \
    --samples "${SAMPLES}" \
    --timeout "${TIMEOUT}" \
    --post-handshake-delay-ms "${POST_HANDSHAKE_DELAY_MS}" \
    --tls-version "${TLS_VERSION}" \
    --out "${RAW_OUT}"
cleanup_pid "${WOLFSSL_PID}"
WOLFSSL_PID=""

echo "[4/6] Running Caddy sweep"
LISTEN_ADDR="${HOST}:${CADDY_PORT}" BENCH_PATH="${BENCH_PATH}" TLS_VERSION="${TLS_VERSION}" \
    "${SCRIPT_DIR}/run_bench_caddy.sh" \
    >"${LOG_DIR}/caddy_server.log" 2>&1 &
CADDY_PID="$!"
wait_for_port "${HOST}" "${CADDY_PORT}" "${CADDY_PID}" "${LOG_DIR}/caddy_server.log"
"${PYTHON_BIN}" "${CLIENT_SCRIPT}" \
    --implementation caddy \
    --host "${HOST}" \
    --port "${CADDY_PORT}" \
    --path "${BENCH_PATH}" \
    --sizes "${SIZES}" \
    --samples "${SAMPLES}" \
    --timeout "${TIMEOUT}" \
    --post-handshake-delay-ms "${POST_HANDSHAKE_DELAY_MS}" \
    --tls-version "${TLS_VERSION}" \
    --out "${RAW_OUT}" \
    --append
cleanup_pid "${CADDY_PID}"
CADDY_PID=""

echo "[5/6] Summarizing results"
"${PYTHON_BIN}" "${SUMMARY_SCRIPT}" --in "${RAW_OUT}" --out "${SUMMARY_OUT}"

echo "[6/6] Done"
echo "Raw CSV:     ${RAW_OUT}"
echo "Summary CSV: ${SUMMARY_OUT}"
echo "Logs:        ${LOG_DIR}"