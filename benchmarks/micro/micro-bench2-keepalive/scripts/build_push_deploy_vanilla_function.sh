#!/usr/bin/env bash
# build_push_deploy_vanilla_function.sh
#
# Builds the bench2 vanilla function image for linux/arm64, pushes it to ttl.sh
# under a unique tag, and deploys bench2-fn-a / bench2-fn-b on the Pi through
# the vanilla OpenFaaS gateway.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"
DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/vanilla_function/Dockerfile"

: "${PI_SSH:=romero@192.168.2.2}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${IMAGE_NAME:=bench2-vanilla-fn}"
: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${GATEWAY_URL:=http://127.0.0.1:8080}"

if [[ -z "${IMAGE_REF:-}" ]]; then
  IMAGE_REF="ttl.sh/${IMAGE_NAME}-$(date +%Y%m%d%H%M%S):24h"
fi

echo "[1/3] ensuring buildx builder: ${BUILDER_NAME}"
if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
  docker buildx create --name "${BUILDER_NAME}" --platform linux/arm64 --use
else
  docker buildx use "${BUILDER_NAME}"
fi

echo "[2/3] building and pushing ${IMAGE_REF}"
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  --progress "${BUILD_PROGRESS}" \
  -f "${DOCKERFILE}" \
  -t "${IMAGE_REF}" \
  --push \
  "${REPO_ROOT}"

echo "[3/3] deploying ${FUNCTION_A} and ${FUNCTION_B} on ${PI_SSH}"
ssh "${PI_SSH}" "
  set -euo pipefail
  faas-cli remove '${FUNCTION_A}' --gateway '${GATEWAY_URL}' >/dev/null 2>&1 || true
  faas-cli remove '${FUNCTION_B}' --gateway '${GATEWAY_URL}' >/dev/null 2>&1 || true

  faas-cli deploy --image '${IMAGE_REF}' --name '${FUNCTION_A}' --gateway '${GATEWAY_URL}' \
    --env BENCH2_WORKER_NAME='${FUNCTION_A}' \
    --env BENCH2_LISTEN_PORT=8080 \
    --label com.openfaas.scale.zero=false

  faas-cli deploy --image '${IMAGE_REF}' --name '${FUNCTION_B}' --gateway '${GATEWAY_URL}' \
    --env BENCH2_WORKER_NAME='${FUNCTION_B}' \
    --env BENCH2_LISTEN_PORT=8080 \
    --label com.openfaas.scale.zero=false

  faas-cli describe '${FUNCTION_A}' --gateway '${GATEWAY_URL}'
"

echo
echo "IMAGE_REF=${IMAGE_REF}"
echo "[ok] vanilla functions deployed"