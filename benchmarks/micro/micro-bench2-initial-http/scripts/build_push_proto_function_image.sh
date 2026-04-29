#!/usr/bin/env bash
# Build and push the prototype timing-fn worker image using the same buildx
# builder as the gateway.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILDER_NAME="${BUILDER_NAME:-default}"
BUILD_PROGRESS="${BUILD_PROGRESS:-plain}"
IMAGE_REF="${IMAGE_REF:-ttl.sh/timing-fn-httpmigrate-bench2:24h}"
CACHE_DIR="${CACHE_DIR:-$ROOT_DIR/.buildx-cache/faasd-gateway}"
USE_LOCAL_CACHE="${USE_LOCAL_CACHE:-auto}"
DOCKERFILE_SRC="$ROOT_DIR/benchmarks/micro/micro-bench2-initial-http/proto_function/timing-fn/Dockerfile"
DOCKERFILE_TMP="$(mktemp /tmp/timing-fn-proto-dockerfile.XXXXXX)"

cleanup() {
  local tmp_cache
  tmp_cache="${CACHE_DIR}.tmp"
  if [ -d "$tmp_cache" ]; then
    rm -rf "$CACHE_DIR"
    mv "$tmp_cache" "$CACHE_DIR"
  fi
  rm -f "$DOCKERFILE_TMP"
}

trap cleanup EXIT

mkdir -p "$CACHE_DIR"

sed '1{/^# syntax=/d;}' "$DOCKERFILE_SRC" > "$DOCKERFILE_TMP"

if ! BUILDER_INSPECT="$(docker buildx inspect "$BUILDER_NAME" 2>&1)"; then
  echo "ERROR: failed to inspect buildx builder: $BUILDER_NAME" >&2
  printf '%s\n' "$BUILDER_INSPECT" >&2
  docker buildx ls >&2 || true
  exit 1
fi

BUILDER_DRIVER="$(printf '%s\n' "$BUILDER_INSPECT" | sed -n 's/^Driver:[[:space:]]*//p' | head -n 1 | tr -d '[:space:]')"
if [ -z "$BUILDER_DRIVER" ]; then
  echo "ERROR: could not inspect buildx builder: $BUILDER_NAME" >&2
  printf '%s\n' "$BUILDER_INSPECT" >&2
  exit 1
fi

case "$USE_LOCAL_CACHE" in
  auto)
    if [ "$BUILDER_DRIVER" = "docker" ]; then
      USE_LOCAL_CACHE=0
    else
      USE_LOCAL_CACHE=1
    fi
    ;;
  0|1)
    ;;
  *)
    echo "ERROR: USE_LOCAL_CACHE must be auto, 0, or 1" >&2
    exit 1
    ;;
esac

echo "[build] builder=$BUILDER_NAME driver=$BUILDER_DRIVER local_cache=$USE_LOCAL_CACHE image=$IMAGE_REF"

build_cmd=(docker buildx build)
build_cmd+=(--builder "$BUILDER_NAME")
build_cmd+=(--progress "$BUILD_PROGRESS")
if [ "$USE_LOCAL_CACHE" = "1" ]; then
  if [ -f "$CACHE_DIR/index.json" ]; then
    build_cmd+=(--cache-from "type=local,src=$CACHE_DIR")
  fi
  build_cmd+=(--cache-to "type=local,dest=${CACHE_DIR}.tmp,mode=max")
fi

build_cmd+=(
  --platform linux/arm64
  -f "$DOCKERFILE_TMP"
  -t "$IMAGE_REF"
  --push
  "$ROOT_DIR"
)

"${build_cmd[@]}"

echo "[ok] pushed $IMAGE_REF"
