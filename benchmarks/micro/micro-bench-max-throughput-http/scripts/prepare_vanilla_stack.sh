#!/usr/bin/env bash
# Deploy the vanilla Keep-Alive stack to the Pi.
# Two standard OpenFaaS functions: timing-fn-a and timing-fn-b
# The vanilla gateway routes by /function/<name> — no relay needed.
set -euo pipefail

BENCH="micro-bench-max-throughput-http"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-tchiaze2003}"
BUILDER_NAME="${BUILDER_NAME:-bench2-arm64-builder}"
OPENFAAS_URL="${OPENFAAS_URL:-http://127.0.0.1:8080}"

FN_A_IMAGE="${FN_A_IMAGE:-ttl.sh/timing-fn-a-vanilla-ka:24h}"
FN_B_IMAGE="${FN_B_IMAGE:-ttl.sh/timing-fn-b-vanilla-ka:24h}"

echo "=== [bench3-ka-vanilla] 1. Building vanilla function images ==="
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/${BENCH}/function/timing-fn-a/Dockerfile" \
  -t "${FN_A_IMAGE}" \
  --push \
  "${ROOT_DIR}"

docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  -f "${ROOT_DIR}/benchmarks/micro/${BENCH}/function/timing-fn-b/Dockerfile" \
  -t "${FN_B_IMAGE}" \
  --push \
  "${ROOT_DIR}"

echo ""
echo "=== [bench3-ka-vanilla] 2. Enabling vanilla benchmark gateway listener ==="
PI_SSH="$PI_SSH" \
PI_SUDO_PASSWORD="$PI_SUDO_PASSWORD" \
BUILDER_NAME="$BUILDER_NAME" \
bash "${ROOT_DIR}/benchmarks/micro/${BENCH}/scripts/build_copy_enable_vanilla_gateway.sh"

echo ""
echo "=== [bench3-ka-vanilla] 3. Deploying timing-fn-a and timing-fn-b ==="
ssh "${PI_SSH}" "
export OPENFAAS_URL='${OPENFAAS_URL}'

faas-cli remove timing-fn-a --gateway \"\$OPENFAAS_URL\" 2>/dev/null || true
faas-cli remove timing-fn-b --gateway \"\$OPENFAAS_URL\" 2>/dev/null || true
sleep 2

faas-cli deploy \
  --image '${FN_A_IMAGE}' \
  --name timing-fn-a \
  --gateway \"\$OPENFAAS_URL\" \
  --env BENCH2_WORKER_NAME=timing-fn-a

faas-cli deploy \
  --image '${FN_B_IMAGE}' \
  --name timing-fn-b \
  --gateway \"\$OPENFAAS_URL\" \
  --env BENCH2_WORKER_NAME=timing-fn-b
"

echo ""
echo "=== [bench3-ka-vanilla] 4. End-to-end tests ==="
sleep 5
curl -sf "http://192.168.2.2:8082/function/timing-fn-a" -X POST -H "Content-Length: 0" \
  && echo "[ok] Vanilla timing-fn-a works!" \
  || echo "[warn] timing-fn-a not yet responding"

curl -sf "http://192.168.2.2:8082/function/timing-fn-b" -X POST -H "Content-Length: 0" \
  && echo "[ok] Vanilla timing-fn-b works!" \
  || echo "[warn] timing-fn-b not yet responding"

echo ""
echo "[ok] Vanilla Keep-Alive stack ready"
echo "    Vanilla gateway: http://192.168.2.2:8082/function/timing-fn-{a,b}"
echo "    OpenFaaS:        http://192.168.2.2:8080"
