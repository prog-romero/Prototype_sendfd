#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"

LISTEN_ADDR="${LISTEN_ADDR:-0.0.0.0:8443}"
UPSTREAM_ADDR="${UPSTREAM_ADDR:-127.0.0.1:8080}"
CERT_FILE="${CERT_FILE:-${REPO_ROOT}/libtlspeek/certs/server.crt}"
KEY_FILE="${KEY_FILE:-${REPO_ROOT}/libtlspeek/certs/server.key}"

if [[ ! -f "${REPO_ROOT}/wolfssl/src/.libs/libwolfssl.so" ]]; then
  echo "wolfSSL not built at: ${REPO_ROOT}/wolfssl/src/.libs/libwolfssl.so" >&2
  echo "Build it first (example):" >&2
  echo "  cd ${REPO_ROOT}/wolfssl && ./autogen.sh && ./configure --enable-all --enable-sessionexport --enable-sni --enable-tls13 && make -j\$(nproc)" >&2
  exit 1
fi

if [[ ! -f "${CERT_FILE}" ]]; then
  echo "Missing CERT_FILE: ${CERT_FILE}" >&2
  exit 1
fi

if [[ ! -f "${KEY_FILE}" ]]; then
  echo "Missing KEY_FILE: ${KEY_FILE}" >&2
  exit 1
fi

make -C "${SCRIPT_DIR}" clean all

exec "${SCRIPT_DIR}/tls_proxy" \
  --listen "${LISTEN_ADDR}" \
  --upstream "${UPSTREAM_ADDR}" \
  --cert "${CERT_FILE}" \
  --key "${KEY_FILE}"
