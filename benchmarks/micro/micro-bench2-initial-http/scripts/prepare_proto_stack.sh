#!/usr/bin/env bash
# Deploy the full prototype HTTP stack to the Pi.
# Gateway image: docker.io/local/faasd-gateway-bench2-initial-http:arm64 (built by build_copy_enable_proto_gateway.sh)
# Worker image:  ttl.sh/timing-fn-httpmigrate-bench2:24h (built here and pushed to ttl.sh)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
# Worker image (separate from gateway image)
WORKER_IMAGE="${WORKER_IMAGE:-ttl.sh/timing-fn-httpmigrate-bench2:24h}"
OPENFAAS_URL="${OPENFAAS_URL:-http://127.0.0.1:8080}"
BUILDER_NAME="${BUILDER_NAME:-bench2-arm64-builder}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-tchiaze2003}"

echo "[proto-http] 1. Building, copying, and enabling proto gateway"
# build_copy_enable_proto_gateway.sh manages its own IMAGE_REF (the gateway image)
# We do NOT pass IMAGE_REF here so it uses its own default:
#   docker.io/local/faasd-gateway-bench2-initial-http:arm64
PI_SSH="$PI_SSH" \
PI_SUDO_PASSWORD="$PI_SUDO_PASSWORD" \
BUILDER_NAME="$BUILDER_NAME" \
bash "${ROOT_DIR}/benchmarks/micro/micro-bench2-initial-http/scripts/build_copy_enable_proto_gateway.sh"

echo "[proto-http] 2. Waiting for gateway to be ready..."
ssh "${PI_SSH}" '
for i in $(seq 1 45); do
  if curl -sf http://127.0.0.1:8080/healthz >/dev/null 2>&1; then
    echo "[ok] Gateway is healthy"
    exit 0
  fi
  echo "Waiting for gateway... ($i/45)"
  sleep 3
done
echo "ERROR: Gateway not ready after 135s"
exit 1
'

echo "[proto-http] 3. Building prototype worker image ${WORKER_IMAGE}"
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/micro-bench2-initial-http/proto_function/timing-fn/Dockerfile" \
  -t "${WORKER_IMAGE}" \
  --push \
  "${ROOT_DIR}"

echo "[proto-http] 4. Deploying timing-fn prototype worker (force remove+redeploy for clean mounts)"
# Remove first so faasd-provider re-applies the volume binding
ssh "${PI_SSH}" "
export OPENFAAS_URL='${OPENFAAS_URL}'
faas-cli remove timing-fn --gateway \"\$OPENFAAS_URL\" 2>/dev/null || true
sleep 3
faas-cli deploy \
  --image '${WORKER_IMAGE}' \
  --name timing-fn \
  --gateway \"\$OPENFAAS_URL\" \
  --env HTTPMIGRATE_FUNCTION_NAME=timing-fn \
  --env HTTPMIGRATE_SOCKET_DIR=/run/tlsmigrate
"

echo "[proto-http] 5. Waiting for worker socket to appear on host..."
ssh "${PI_SSH}" "
for i in \$(seq 1 25); do
  if echo '${PI_SUDO_PASSWORD}' | sudo -S -p '' test -S /var/lib/faasd/tlsmigrate/timing-fn.sock 2>/dev/null; then
    echo '[ok] Socket /var/lib/faasd/tlsmigrate/timing-fn.sock exists'
    exit 0
  fi
  echo \"Waiting for worker socket... (\$i/25)\"
  sleep 3
done
echo 'WARNING: socket not found after 75s — worker may still be starting'
"

echo "[proto-http] 6. End-to-end test"
curl -sf "http://192.168.2.2:8083/function/timing-fn" -X POST -H "Content-Length: 0" && echo "[ok] Proto HTTP works!" || echo "[warn] Proto HTTP not yet responding — try running the benchmark"

echo ""
echo "[ok] Prototype HTTP stack ready"
echo "    Proto gateway:  http://${PI_HOST:-192.168.2.2}:8083/function/timing-fn"
echo "    OpenFaaS:       http://${PI_HOST:-192.168.2.2}:8080"