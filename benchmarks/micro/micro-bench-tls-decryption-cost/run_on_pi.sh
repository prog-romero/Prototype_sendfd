#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
RESULTS_DIR="${RESULTS_DIR:-${SCRIPT_DIR}/results}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

: "${ITERATIONS:=1000}"
: "${WARMUP:=100}"
: "${PAYLOAD_SIZES:=64,256,1024,4096,16384,65536,131072,262144,524288,1048576}"
: "${PAYLOAD_LABELS:=64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, 64 KiB, 128 KiB, 256 KiB, 512 KiB, 1 MiB}"
: "${TLS_VERSION:=1.3}"
: "${CIPHER:=}"
: "${CPU_CORE:=}"

: "${SERVER_CERT:=${REPO_ROOT}/libtlspeek/certs/server.crt}"
: "${SERVER_KEY:=${REPO_ROOT}/libtlspeek/certs/server.key}"
: "${CA_CERT:=${REPO_ROOT}/libtlspeek/certs/ca.crt}"

mkdir -p "${RESULTS_DIR}"

SUMMARY_OUT="${RESULTS_DIR}/decrypt_summary_${TIMESTAMP}.csv"
RAW_OUT="${RESULTS_DIR}/decrypt_raw_${TIMESTAMP}.csv"

echo "[decrypt-cost] Building benchmark..."
make -C "${SCRIPT_DIR}" clean all

cmd=(
  "${SCRIPT_DIR}/bench_tls_decrypt_memio"
  --iterations "${ITERATIONS}"
  --warmup "${WARMUP}"
  --sizes "${PAYLOAD_SIZES}"
  --tls-version "${TLS_VERSION}"
  --server-cert "${SERVER_CERT}"
  --server-key "${SERVER_KEY}"
  --ca-cert "${CA_CERT}"
  --summary-out "${SUMMARY_OUT}"
  --raw-out "${RAW_OUT}"
)

if [[ -n "${CIPHER}" ]]; then
  cmd+=(--cipher "${CIPHER}")
fi

if [[ -n "${CPU_CORE}" ]]; then
  cmd+=(--core "${CPU_CORE}")
fi

echo "[decrypt-cost] Running pure TLS receive/decrypt benchmark..."
echo "[decrypt-cost] Payload sizes : ${PAYLOAD_LABELS}"
echo "[decrypt-cost] Payload bytes : ${PAYLOAD_SIZES}"
echo "[decrypt-cost] Iterations    : ${ITERATIONS}"
echo "[decrypt-cost] Warmup        : ${WARMUP}"
echo "[decrypt-cost] TLS version   : ${TLS_VERSION}"
if [[ -n "${CIPHER}" ]]; then
  echo "[decrypt-cost] Cipher        : ${CIPHER}"
fi
if [[ -n "${CPU_CORE}" ]]; then
  echo "[decrypt-cost] CPU core      : ${CPU_CORE}"
fi

"${cmd[@]}"

echo "[decrypt-cost] Summary CSV: ${SUMMARY_OUT}"
echo "[decrypt-cost] Raw CSV    : ${RAW_OUT}"
