#!/usr/bin/env bash
# Deploy the full vanilla HTTP stack to the Pi.
# Gateway image: docker.io/local/faasd-gateway-bench2-initial-http:arm64 (built by build_copy_enable_vanilla_gateway.sh)
# Function image: ttl.sh/timing-fn-bench2-initial-http:24h (built here and pushed to ttl.sh)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
# Use separate variable for function image to avoid collision with gateway's IMAGE_REF
FUNCTION_IMAGE="${FUNCTION_IMAGE:-ttl.sh/timing-fn-bench2-initial-http:24h}"
OPENFAAS_URL="${OPENFAAS_URL:-http://127.0.0.1:8080}"
BUILDER_NAME="${BUILDER_NAME:-bench2-arm64-builder}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-tchiaze2003}"

echo "[vanilla-http] 1. Building timing function image ${FUNCTION_IMAGE}"
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/micro-bench2-initial-http/function/timing_fn/Dockerfile" \
  -t "${FUNCTION_IMAGE}" \
  --push \
  "${ROOT_DIR}"

echo "[vanilla-http] 2. Deploying timing-fn"
ssh "${PI_SSH}" \
  "export OPENFAAS_URL='${OPENFAAS_URL}' && faas-cli deploy --image '${FUNCTION_IMAGE}' --name timing-fn --gateway '${OPENFAAS_URL}'"

echo "[vanilla-http] 3. Enabling benchmark gateway listener on the Pi"
# build_copy_enable_vanilla_gateway.sh manages its own IMAGE_REF (the gateway image)
PI_SSH="$PI_SSH" \
PI_SUDO_PASSWORD="$PI_SUDO_PASSWORD" \
BUILDER_NAME="$BUILDER_NAME" \
bash "${ROOT_DIR}/benchmarks/micro/micro-bench2-initial-http/scripts/build_copy_enable_vanilla_gateway.sh"

echo "[ok] Vanilla HTTP stack ready at http://${PI_HOST:-192.168.2.2}:8082/function/timing-fn"
