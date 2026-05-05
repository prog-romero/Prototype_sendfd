#!/usr/bin/env bash
# run_on_pi.sh — Build and start the bench2 vanilla TLS proxy on the Pi.
#
# Run as: bash run_on_pi.sh [--upstream HOST:PORT]
# Default upstream is 127.0.0.1:8080 (local faasd gateway).
#
# Prerequisites on the Pi:
#   wolfSSL installed to /usr/local (--enable-tls13 --enable-opensslextra
#                                     --enable-aesgcm --enable-chacha)
#   Certs default to ~/Prototype_sendfd/libtlspeek/certs/ (no /certs/ dir needed)
#   Override with: BENCH2_CERT=/path/to/server.crt BENCH2_KEY=/path/to/server.key

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPSTREAM="${1:-127.0.0.1:8080}"

# Default: use certs from the libtlspeek directory in the project
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
: "${BENCH2_CERT:=${REPO_ROOT}/libtlspeek/certs/server.crt}"
: "${BENCH2_KEY:=${REPO_ROOT}/libtlspeek/certs/server.key}"

if [[ ! -f "${BENCH2_CERT}" ]]; then
    echo "[run_on_pi] ERROR: cert not found: ${BENCH2_CERT}" >&2
    exit 1
fi
if [[ ! -f "${BENCH2_KEY}" ]]; then
    echo "[run_on_pi] ERROR: key not found: ${BENCH2_KEY}" >&2
    exit 1
fi

echo "[run_on_pi] Building bench2_vanilla_proxy..."
make -C "${SCRIPT_DIR}" clean all

echo "[run_on_pi] Using cert: ${BENCH2_CERT}"
echo "[run_on_pi] Starting bench2_vanilla_proxy on :8444 → ${UPSTREAM}"
exec "${SCRIPT_DIR}/bench2_vanilla_proxy" \
    --listen   "0.0.0.0:8444" \
    --upstream "${UPSTREAM}" \
    --cert     "${BENCH2_CERT}" \
    --key      "${BENCH2_KEY}"
