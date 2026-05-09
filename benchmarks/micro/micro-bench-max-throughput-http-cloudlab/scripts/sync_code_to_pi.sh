#!/usr/bin/env bash
# Sync the bench2 benchmark and provider support tree to the Pi and verify
# that both sides have identical file contents for the synced paths.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
PI_USER="${PI_SSH%@*}"
REMOTE_ROOT="${REMOTE_ROOT:-/home/${PI_USER}/Prototype_sendfd}"

SYNC_PATHS=(
  "benchmarks/micro/micro-bench3-keepalive-http"
)

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "ERROR: required command not found: $1" >&2
    exit 1
  fi
}

verify_tree() {
  local rel_path="$1"
  local local_manifest remote_manifest

  local_manifest="$(mktemp)"
  remote_manifest="$(mktemp)"

  (
    cd "$ROOT_DIR/$rel_path"
    find . -type f -exec sha256sum {} + | sort
  ) > "$local_manifest"

  ssh "$PI_SSH" "cd '$REMOTE_ROOT/$rel_path' && find . -type f -exec sha256sum {} + | sort" > "$remote_manifest"

  if ! diff -u "$local_manifest" "$remote_manifest" >/dev/null; then
    echo "ERROR: content mismatch after sync for $rel_path" >&2
    diff -u "$local_manifest" "$remote_manifest" || true
    rm -f "$local_manifest" "$remote_manifest"
    exit 1
  fi

  rm -f "$local_manifest" "$remote_manifest"
  echo "[ok] verified $rel_path"
}

require_cmd ssh
require_cmd rsync
require_cmd sha256sum
require_cmd find
require_cmd sort
require_cmd diff

echo "[init] root=$ROOT_DIR pi=$PI_SSH remote_root=$REMOTE_ROOT"
ssh "$PI_SSH" "mkdir -p '$REMOTE_ROOT/benchmarks/micro'"

for rel_path in "${SYNC_PATHS[@]}"; do
  echo "[sync] $rel_path"
  rsync -az --delete "$ROOT_DIR/$rel_path/" "$PI_SSH:$REMOTE_ROOT/$rel_path/"
  verify_tree "$rel_path"
done

echo "[ok] Pi source tree is aligned for the bench2 HTTP benchmark"
