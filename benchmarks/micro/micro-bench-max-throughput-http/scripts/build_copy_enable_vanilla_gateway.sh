#!/usr/bin/env bash
# Build the bench2 HTTP gateway image for ARM64, export it as a Docker archive,
# copy it to the Pi, import it into containerd, and optionally enable the
# vanilla HTTP benchmark mode.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-}"
IMAGE_REF="${IMAGE_REF:-docker.io/local/faasd-gateway-bench3-ka-http:arm64}"
LOCAL_ARCHIVE="${LOCAL_ARCHIVE:-$ROOT_DIR/dist/faasd-gateway-bench3-ka-http-arm64.tar}"
REMOTE_ARCHIVE="${REMOTE_ARCHIVE:-/tmp/faasd-gateway-bench3-ka-http-arm64.tar}"
CACHE_DIR="${CACHE_DIR:-$ROOT_DIR/.buildx-cache/faasd-gateway}"
ENABLE_ON_PI="${ENABLE_ON_PI:-1}"
KEEP_REMOTE_ARCHIVE="${KEEP_REMOTE_ARCHIVE:-0}"
BUILDER_NAME="${BUILDER_NAME:-bench2-arm64-builder}"
USE_LOCAL_CACHE="${USE_LOCAL_CACHE:-auto}"
BUILD_PROGRESS="${BUILD_PROGRESS:-plain}"

DOCKERFILE_SRC="$ROOT_DIR/benchmarks/micro/micro-bench3-keepalive-http/proto_gateway/Dockerfile"
DOCKERFILE_TMP="$(mktemp /tmp/faasd-gateway-dockerfile.XXXXXX)"

mkdir -p "$(dirname "$LOCAL_ARCHIVE")" "$CACHE_DIR"

echo "[init] root=$ROOT_DIR builder=$BUILDER_NAME pi=$PI_SSH"

# Do NOT strip the # syntax= line — it is required by docker buildx
cp "$DOCKERFILE_SRC" "$DOCKERFILE_TMP"

# Ensure the builder exists with the docker-container driver (supports ARM64 emulation)
if ! docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
  echo "[init] creating buildx builder: $BUILDER_NAME"
  docker buildx create --name "$BUILDER_NAME" --driver docker-container --bootstrap
fi

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

cleanup_cache() {
  local tmp_cache
  tmp_cache="${CACHE_DIR}.tmp"
  if [ -d "$tmp_cache" ]; then
    rm -rf "$CACHE_DIR"
    mv "$tmp_cache" "$CACHE_DIR"
  fi
  rm -f "$DOCKERFILE_TMP"
}

trap cleanup_cache EXIT

build_cmd=(docker buildx build)
if [ -n "$BUILDER_NAME" ]; then
  build_cmd+=(--builder "$BUILDER_NAME")
fi
if [ -n "$BUILD_PROGRESS" ]; then
  build_cmd+=(--progress "$BUILD_PROGRESS")
fi
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
  "$ROOT_DIR"
)

rm -f "$LOCAL_ARCHIVE"

if [ "$BUILDER_DRIVER" = "docker" ]; then
  "${build_cmd[@]}"
  docker image save -o "$LOCAL_ARCHIVE" "$IMAGE_REF"
else
  build_cmd+=(--output "type=docker,dest=$LOCAL_ARCHIVE")
  "${build_cmd[@]}"
fi

scp "$LOCAL_ARCHIVE" "$PI_SSH:$REMOTE_ARCHIVE"
ssh "$PI_SSH" "set -e; printf '%s\n' '$PI_SUDO_PASSWORD' | sudo -S -p '' ctr -n openfaas images import '$REMOTE_ARCHIVE'; printf '%s\n' '$PI_SUDO_PASSWORD' | sudo -S -p '' ctr -n openfaas images ls | grep -F '$IMAGE_REF'"

if [ "$KEEP_REMOTE_ARCHIVE" != "1" ]; then
  ssh "$PI_SSH" "rm -f '$REMOTE_ARCHIVE'"
fi

if [ "$ENABLE_ON_PI" = "1" ]; then
  ssh "$PI_SSH" "cd ~/Prototype_sendfd && PI_SUDO_PASSWORD='$PI_SUDO_PASSWORD' BENCH_GATEWAY_IMAGE='$IMAGE_REF' bash benchmarks/micro/micro-bench3-keepalive-http/scripts/pi_enable_vanilla_gateway.sh"
else
  echo "[info] image imported. To enable it later on the Pi:"
  echo "  cd ~/Prototype_sendfd && BENCH_GATEWAY_IMAGE=$IMAGE_REF bash benchmarks/micro/micro-bench3-keepalive-http/scripts/pi_enable_vanilla_gateway.sh"
fi

echo "[ok] gateway image built, copied, and imported"
