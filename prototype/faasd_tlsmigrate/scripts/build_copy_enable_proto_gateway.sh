#!/usr/bin/env bash
# Build the prototype gateway image for ARM64, export it as a Docker archive,
# copy it to the Pi, import it into containerd, and optionally enable it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
IMAGE_REF="${IMAGE_REF:-docker.io/local/faasd-gateway-tlsmigrate:arm64}"
LOCAL_ARCHIVE="${LOCAL_ARCHIVE:-$ROOT_DIR/dist/faasd-gateway-tlsmigrate-arm64.tar}"
REMOTE_ARCHIVE="${REMOTE_ARCHIVE:-/tmp/faasd-gateway-tlsmigrate-arm64.tar}"
CACHE_DIR="${CACHE_DIR:-$ROOT_DIR/.buildx-cache/faasd-gateway}"
ENABLE_ON_PI="${ENABLE_ON_PI:-1}"
KEEP_REMOTE_ARCHIVE="${KEEP_REMOTE_ARCHIVE:-0}"
BUILDER_NAME="${BUILDER_NAME:-default}"
USE_LOCAL_CACHE="${USE_LOCAL_CACHE:-auto}"
BUILD_PROGRESS="${BUILD_PROGRESS:-plain}"

DOCKERFILE_SRC="$ROOT_DIR/prototype/faasd_tlsmigrate/gateway/Dockerfile"
DOCKERFILE_TMP="$(mktemp /tmp/faasd-gateway-dockerfile.XXXXXX)"

mkdir -p "$(dirname "$LOCAL_ARCHIVE")" "$CACHE_DIR"

echo "[init] root=$ROOT_DIR builder=$BUILDER_NAME pi=$PI_SSH"

sed '1{/^# syntax=/d;}' "$DOCKERFILE_SRC" > "$DOCKERFILE_TMP"

if ! BUILDER_INSPECT="$(docker buildx inspect "$BUILDER_NAME" 2>&1)"; then
  echo "ERROR: failed to inspect buildx builder: $BUILDER_NAME" >&2
  printf '%s\n' "$BUILDER_INSPECT" >&2
  echo "[hint] available builders:" >&2
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

echo "[build] exporting $IMAGE_REF to $LOCAL_ARCHIVE"
echo "[build] builder=$BUILDER_NAME driver=$BUILDER_DRIVER local_cache=$USE_LOCAL_CACHE progress=$BUILD_PROGRESS"
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
  echo "[build] docker driver detected; building into local image store first"
  "${build_cmd[@]}"
  echo "[save] writing Docker image archive to $LOCAL_ARCHIVE"
  docker image save -o "$LOCAL_ARCHIVE" "$IMAGE_REF"
else
  build_cmd+=(--output "type=docker,dest=$LOCAL_ARCHIVE")
  "${build_cmd[@]}"
fi

echo "[scp] $LOCAL_ARCHIVE -> $PI_SSH:$REMOTE_ARCHIVE"
scp "$LOCAL_ARCHIVE" "$PI_SSH:$REMOTE_ARCHIVE"

echo "[ssh] importing $IMAGE_REF into containerd namespace openfaas"
ssh -t "$PI_SSH" "set -e; sudo ctr -n openfaas images import '$REMOTE_ARCHIVE'; sudo ctr -n openfaas images ls | grep -F '$IMAGE_REF'"

if [ "$KEEP_REMOTE_ARCHIVE" != "1" ]; then
  echo "[ssh] removing $REMOTE_ARCHIVE from the Pi"
  ssh "$PI_SSH" "rm -f '$REMOTE_ARCHIVE'"
fi

if [ "$ENABLE_ON_PI" = "1" ]; then
  echo "[ssh] enabling prototype gateway with image $IMAGE_REF"
  ssh -t "$PI_SSH" "cd ~/Prototype_sendfd && PROTO_GATEWAY_IMAGE='$IMAGE_REF' bash prototype/faasd_tlsmigrate/scripts/pi_enable_proto_gateway.sh"
else
  echo "[info] image imported. To enable it later on the Pi:"
  echo "  cd ~/Prototype_sendfd && PROTO_GATEWAY_IMAGE=$IMAGE_REF bash prototype/faasd_tlsmigrate/scripts/pi_enable_proto_gateway.sh"
fi

echo "[ok] gateway image built, copied, and imported"