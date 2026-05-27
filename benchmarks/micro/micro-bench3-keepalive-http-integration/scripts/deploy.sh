#!/usr/bin/env bash
# deploy.sh — Deploy timing-fn-a and timing-fn-b for the keepalive integration bench.
#
# Prerequisites:
#   - faas-cli installed and logged in to the gateway.
#   - Docker images timing-fn-a-ka-integration:latest and
#     timing-fn-b-ka-integration:latest already pushed to the registry.
#   - Environment variables:
#       GATEWAY   — OpenFaaS gateway URL  (default: http://127.0.0.1:8080)
#       REGISTRY  — Docker registry prefix (default: "")
#
# Usage:
#   GATEWAY=http://pi.local:8080 REGISTRY=myregistry.io/bench bash deploy.sh

set -euo pipefail

GATEWAY="${GATEWAY:-http://127.0.0.1:8080}"
REGISTRY="${REGISTRY:-}"

IMAGE_PREFIX="${REGISTRY:+${REGISTRY}/}"

FN_A_IMAGE="${IMAGE_PREFIX}timing-fn-a-ka-integration:latest"
FN_B_IMAGE="${IMAGE_PREFIX}timing-fn-b-ka-integration:latest"

SEND_FD_SOCKET_DIR="/run/tlsmigrate"

deploy_fn() {
    local fn_name="$1"
    local image="$2"

    echo "[deploy] deploying ${fn_name} from image ${image} ..."

    faas-cli deploy \
        --gateway "${GATEWAY}" \
        --image   "${image}" \
        --name    "${fn_name}" \
        --env     "HTTPMIGRATE_ENABLE=1" \
        --env     "SENDFD_ENABLE=1" \
        --env     "SENDFD_SOCKET_DIR=${SEND_FD_SOCKET_DIR}" \
        --env     "HTTPMIGRATE_KA_FUNCTION_NAME=${fn_name}" \
        --env     "sendfd_enable=1" \
        --env     "sendfd_socket_dir=${SEND_FD_SOCKET_DIR}" \
        --fprocess "/usr/local/bin/timing-fn-ka-worker"

    echo "[deploy] ${fn_name} deployed."
}

echo "[deploy] Gateway: ${GATEWAY}"

deploy_fn "timing-fn-a" "${FN_A_IMAGE}"
deploy_fn "timing-fn-b" "${FN_B_IMAGE}"

echo "[deploy] All functions deployed."
echo ""
echo "Run a quick smoke test:"
echo "  curl -s -X POST ${GATEWAY}/function/timing-fn-a -d 'hello' | python3 -m json.tool"
echo "  curl -s -X POST ${GATEWAY}/function/timing-fn-b -d 'hello' | python3 -m json.tool"
