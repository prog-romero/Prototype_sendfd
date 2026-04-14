#!/usr/bin/env bash
# Builds a patched faasd binary (for the faasd-provider systemd service)
# that bind-mounts a shared /run/tlsmigrate directory into function containers.
#
# Output: dist/faasd.tlsmigrate.linux-arm64
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PATCH_FILE="${REPO_ROOT}/prototype/faasd_tlsmigrate/provider/patches/openfaas-faasd-provider-tlsmigrate.patch"
OUT_DIR="${REPO_ROOT}/dist"
OUT_BIN="${OUT_DIR}/faasd.tlsmigrate.linux-arm64"

FAASD_COMMIT_DEFAULT="13f03f3d47072f29781ef78a1f5a406c800cc513"
FAASD_COMMIT="${FAASD_COMMIT:-$FAASD_COMMIT_DEFAULT}"

mkdir -p "$OUT_DIR"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[build] cloning openfaas/faasd @ ${FAASD_COMMIT}"
git clone https://github.com/openfaas/faasd "$TMP/faasd" >/dev/null
cd "$TMP/faasd"
git checkout "$FAASD_COMMIT" >/dev/null

echo "[build] applying patch: ${PATCH_FILE}"
git apply "$PATCH_FILE"

echo "[build] building linux/arm64 binary"
if command -v go >/dev/null 2>&1; then
  CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
    go build -buildvcs=false -mod=vendor -trimpath -ldflags "-s -w" -o "$OUT_BIN" .
elif command -v docker >/dev/null 2>&1; then
  echo "[build] local 'go' not found, using Dockerized Go toolchain"
  docker run --rm \
    -e CGO_ENABLED=0 \
    -e GOOS=linux \
    -e GOARCH=arm64 \
    -v "$TMP/faasd:/src" \
    -v "$OUT_DIR:/out" \
    -w /src \
    golang:1.24-bookworm \
    go build -buildvcs=false -mod=vendor -trimpath -ldflags "-s -w" -o "/out/$(basename "$OUT_BIN")" .
else
  echo "ERROR: neither 'go' nor 'docker' is available on this machine" >&2
  echo "Install Go or Docker, then re-run this script." >&2
  exit 1
fi

echo "[build] OK: $OUT_BIN"
