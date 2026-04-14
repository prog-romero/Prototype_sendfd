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

mkdir -p "$(dirname "$LOCAL_ARCHIVE")" "$CACHE_DIR"

cleanup_cache() {
  local tmp_cache
  tmp_cache="${CACHE_DIR}.tmp"
  if [ -d "$tmp_cache" ]; then
    rm -rf "$CACHE_DIR"
    mv "$tmp_cache" "$CACHE_DIR"
  fi
}

trap cleanup_cache EXIT

echo "[build] exporting $IMAGE_REF to $LOCAL_ARCHIVE"
docker buildx build \
  --platform linux/arm64 \
  -f "$ROOT_DIR/prototype/faasd_tlsmigrate/gateway/Dockerfile" \
  -t "$IMAGE_REF" \
  --cache-from "type=local,src=$CACHE_DIR" \
  --cache-to "type=local,dest=${CACHE_DIR}.tmp,mode=max" \
  --output "type=docker,dest=$LOCAL_ARCHIVE" \
  "$ROOT_DIR"

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