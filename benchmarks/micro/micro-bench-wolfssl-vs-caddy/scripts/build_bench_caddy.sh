#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
MODULE_DIR="${ROOT_DIR}/caddy_bench"
BIN_DIR="${MODULE_DIR}/bin"
BIN_PATH="${BIN_DIR}/benchcaddy"

if [[ -x /usr/local/go/bin/go && ":${PATH}:" != *":/usr/local/go/bin:"* ]]; then
    export PATH="/usr/local/go/bin:${PATH}"
fi

if ! command -v go >/dev/null 2>&1; then
    echo "ERROR: Go was not found. Run scripts/install_local_caddy_env.sh first." >&2
    exit 1
fi

mkdir -p "${BIN_DIR}"

cd "${MODULE_DIR}"
go mod tidy
go build -o "${BIN_PATH}" ./cmd/benchcaddy

echo "Built ${BIN_PATH}"