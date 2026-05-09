#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
CLIENT_DIR="$ROOT_DIR/benchmarks/micro/micro-bench2-initial-http/client"
RUNNER="$CLIENT_DIR/run_payload_sweep.py"

HOST="${1:-${HOST:-}}"
PORT="${PORT:-8083}"
REQUESTS="${REQUESTS:-50}"
TIMEOUT="${TIMEOUT:-120}"

if [[ -z "$HOST" ]]; then
  echo "Usage: $0 <host>" >&2
  echo "Example: $0 192.168.2.2" >&2
  exit 2
fi

run_sweep() {
  local label="$1"
  local start_kb="$2"
  local end_kb="$3"
  local step_kb="$4"
  local out_csv="$5"

  echo "[run] $label"
  echo "[run] host=$HOST port=$PORT start_kb=$start_kb end_kb=$end_kb step_kb=$step_kb requests=$REQUESTS timeout=$TIMEOUT"

  python3 "$RUNNER" \
    --host "$HOST" \
    --port "$PORT" \
    --start-kb "$start_kb" \
    --end-kb "$end_kb" \
    --step-kb "$step_kb" \
    --requests "$REQUESTS" \
    --timeout "$TIMEOUT" \
    --out "$out_csv"

  echo "[ok] wrote $out_csv"
}

run_sweep \
  "step 32 KiB, 32 KiB -> 1024 KiB" \
  32 \
  1024 \
  32 \
  "$CLIENT_DIR/proto_results_step32kb_32_to_1024.csv"

run_sweep \
  "step 100 KiB, 1000 KiB -> 1500 KiB" \
  1000 \
  1500 \
  100 \
  "$CLIENT_DIR/proto_results_step100kb_1000_to_1500.csv"

run_sweep \
  "step 1000 KiB, 1500 KiB -> 60500 KiB" \
  1500 \
  60500 \
  1000 \
  "$CLIENT_DIR/proto_results_step1000kb_1500_to_60500.csv"

echo "[ok] all prototype sweep sets completed"
