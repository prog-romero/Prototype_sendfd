#!/usr/bin/env bash
# Deploy the full keep-alive prototype HTTP stack to the Pi.
# Gateway: builds the patched gateway with relay support
# Workers: timing-fn-a and timing-fn-b with keep-alive loop
set -euo pipefail

BENCH="micro-bench3-keepalive-http"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-tchiaze2003}"
BUILDER_NAME="${BUILDER_NAME:-bench2-arm64-builder}"
OPENFAAS_URL="${OPENFAAS_URL:-http://127.0.0.1:8080}"

WORKER_A_IMAGE="${WORKER_A_IMAGE:-ttl.sh/timing-fn-a-ka-http:24h}"
WORKER_B_IMAGE="${WORKER_B_IMAGE:-ttl.sh/timing-fn-b-ka-http:24h}"
GW_IMAGE="${GW_IMAGE:-docker.io/local/faasd-gateway-bench3-ka-http:arm64}"
GW_ARCHIVE="${ROOT_DIR}/dist/faasd-gateway-bench3-ka-http-arm64.tar"

echo "=== [bench3-ka-proto] 1. Building Keep-Alive Prototype Gateway ==="
PI_SSH="$PI_SSH" \
PI_SUDO_PASSWORD="$PI_SUDO_PASSWORD" \
BUILDER_NAME="$BUILDER_NAME" \
bash "${ROOT_DIR}/benchmarks/micro/${BENCH}/scripts/build_copy_enable_proto_gateway.sh"

echo ""
echo "=== [bench3-ka-proto] 2. Waiting for gateway to be ready ==="
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

echo ""
echo "=== [bench3-ka-proto] 3. Building worker images for timing-fn-a and timing-fn-b ==="
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/${BENCH}/proto_function/timing-fn-a/Dockerfile" \
  -t "${WORKER_A_IMAGE}" \
  --push \
  "${ROOT_DIR}"

docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/${BENCH}/proto_function/timing-fn-b/Dockerfile" \
  -t "${WORKER_B_IMAGE}" \
  --push \
  "${ROOT_DIR}"

echo ""
echo "=== [bench3-ka-proto] 4. Deploying timing-fn-a and timing-fn-b ==="
ssh "${PI_SSH}" "
export OPENFAAS_URL='${OPENFAAS_URL}'

# Remove old deployments
faas-cli remove timing-fn-a --gateway \"\$OPENFAAS_URL\" 2>/dev/null || true
faas-cli remove timing-fn-b --gateway \"\$OPENFAAS_URL\" 2>/dev/null || true
sleep 3

# Deploy fn-a
faas-cli deploy \
  --image '${WORKER_A_IMAGE}' \
  --name timing-fn-a \
  --gateway \"\$OPENFAAS_URL\" \
  --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a \
  --env HTTPMIGRATE_KA_SOCKET_DIR=/run/tlsmigrate \
  --env HTTPMIGRATE_KA_RELAY_SOCKET=/run/tlsmigrate/relay.sock

# Deploy fn-b
faas-cli deploy \
  --image '${WORKER_B_IMAGE}' \
  --name timing-fn-b \
  --gateway \"\$OPENFAAS_URL\" \
  --env HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-b \
  --env HTTPMIGRATE_KA_SOCKET_DIR=/run/tlsmigrate \
  --env HTTPMIGRATE_KA_RELAY_SOCKET=/run/tlsmigrate/relay.sock
"

echo ""
echo "=== [bench3-ka-proto] 5. Waiting for worker sockets ==="
ssh "${PI_SSH}" "
for i in \$(seq 1 30); do
  A_OK=0; B_OK=0
  echo '${PI_SUDO_PASSWORD}' | sudo -S -p '' test -S /var/lib/faasd/tlsmigrate/timing-fn-a.sock 2>/dev/null && A_OK=1
  echo '${PI_SUDO_PASSWORD}' | sudo -S -p '' test -S /var/lib/faasd/tlsmigrate/timing-fn-b.sock 2>/dev/null && B_OK=1
  if [ \$A_OK -eq 1 ] && [ \$B_OK -eq 1 ]; then
    echo '[ok] Both worker sockets exist'
    exit 0
  fi
  echo \"Waiting for sockets... fn-a=\$A_OK fn-b=\$B_OK (\$i/30)\"
  sleep 3
done
echo 'WARNING: sockets not found after 90s'
"

echo ""
echo "=== [bench3-ka-proto] 6. End-to-end tests ==="
echo "Testing fn-a..."
curl -sf "http://192.168.2.2:8083/function/timing-fn-a" -X POST -H "Content-Length: 0" \
  && echo "[ok] Proto HTTP timing-fn-a works!" \
  || echo "[warn] timing-fn-a not yet responding"

echo "Testing fn-b..."
curl -sf "http://192.168.2.2:8083/function/timing-fn-b" -X POST -H "Content-Length: 0" \
  && echo "[ok] Proto HTTP timing-fn-b works!" \
  || echo "[warn] timing-fn-b not yet responding"

echo ""
echo "[ok] Keep-Alive Prototype HTTP stack ready"
echo "    Proto gateway:  http://192.168.2.2:8083/function/timing-fn-{a,b}"
echo "    OpenFaaS:       http://192.168.2.2:8080"