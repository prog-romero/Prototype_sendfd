#!/usr/bin/env bash
# build_push_deploy_proto_worker.sh
#
# Builds the bench2 prototype worker image for linux/arm64, pushes it to ttl.sh
# under a unique tag, deploys bench2-fn-a / bench2-fn-b on the Pi, and can
# verify that the running worker binary matches the pushed image.
#
# Environment variables:
#   PI_SSH              SSH target                      (default: romero@192.168.2.2)
#   BUILDER_NAME        buildx builder                 (default: bench2-arm64-builder)
#   BUILD_PROGRESS      plain|auto                     (default: auto)
#   BENCH2_DEBUG        1 keeps verbose tracing on     (default: 0)
#   IMAGE_NAME          ttl.sh base name               (default: bench2-proto-worker)
#   IMAGE_REF           full image ref                 (default: generated unique ttl.sh tag)
#   FUNCTION_A          first OpenFaaS function name   (default: bench2-fn-a)
#   FUNCTION_B          second OpenFaaS function name  (default: bench2-fn-b)
#   GATEWAY_URL         OpenFaaS gateway URL           (default: http://127.0.0.1:8080)
#   VERIFY_HASH         1 to verify live binary hash   (default: 1)
#   PI_SUDO_PASSWORD    sudo password for hash verify  (optional, required for VERIFY_HASH=1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"
DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/proto_worker/Dockerfile"

: "${PI_SSH:=romero@192.168.2.2}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${BENCH2_DEBUG:=0}"
: "${IMAGE_NAME:=bench2-proto-worker}"
: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${GATEWAY_URL:=http://127.0.0.1:8080}"
: "${VERIFY_HASH:=1}"

if [[ -z "${IMAGE_REF:-}" ]]; then
  IMAGE_REF="ttl.sh/${IMAGE_NAME}-$(date +%Y%m%d%H%M%S):24h"
fi

echo "[1/5] ensuring buildx builder: ${BUILDER_NAME}"
if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
  docker buildx create --name "${BUILDER_NAME}" --platform linux/arm64 --use
else
  docker buildx use "${BUILDER_NAME}"
fi

echo "[2/5] building and pushing ${IMAGE_REF}"
docker buildx build \
  --builder "${BUILDER_NAME}" \
  --platform linux/arm64 \
  --progress "${BUILD_PROGRESS}" \
  --build-arg "BENCH2_DEBUG=${BENCH2_DEBUG}" \
  -f "${DOCKERFILE}" \
  -t "${IMAGE_REF}" \
  --push \
  "${REPO_ROOT}"

echo "[3/5] computing expected worker binary hash from pushed image"
EXPECTED_HASH="$(docker run --rm --platform linux/arm64 "${IMAGE_REF}" /bin/sh -lc 'sha256sum /usr/local/bin/bench2_proto_worker | cut -d" " -f1')"
echo "[local] expected hash: ${EXPECTED_HASH}"

echo "[4/5] deploying ${FUNCTION_A} and ${FUNCTION_B} on ${PI_SSH}"
ssh "${PI_SSH}" "
  set -euo pipefail
  faas-cli remove '${FUNCTION_A}' --gateway '${GATEWAY_URL}' >/dev/null 2>&1 || true
  faas-cli remove '${FUNCTION_B}' --gateway '${GATEWAY_URL}' >/dev/null 2>&1 || true

  faas-cli deploy --image '${IMAGE_REF}' --name '${FUNCTION_A}' --gateway '${GATEWAY_URL}' \
    --env BENCH2_FUNCTION_NAME='${FUNCTION_A}' \
    --env BENCH2_SOCKET_DIR=/run/bench2 \
    --env BENCH2_RELAY_SOCKET=/run/bench2/relay.sock \
    --env BENCH2_CERT=/certs/server.crt \
    --env BENCH2_KEY=/certs/server.key \
    --label com.openfaas.scale.zero=false

  faas-cli deploy --image '${IMAGE_REF}' --name '${FUNCTION_B}' --gateway '${GATEWAY_URL}' \
    --env BENCH2_FUNCTION_NAME='${FUNCTION_B}' \
    --env BENCH2_SOCKET_DIR=/run/bench2 \
    --env BENCH2_RELAY_SOCKET=/run/bench2/relay.sock \
    --env BENCH2_CERT=/certs/server.crt \
    --env BENCH2_KEY=/certs/server.key \
    --label com.openfaas.scale.zero=false

  faas-cli describe '${FUNCTION_A}' --gateway '${GATEWAY_URL}'
"

if [[ "${VERIFY_HASH}" == "1" ]]; then
  if [[ -z "${PI_SUDO_PASSWORD:-}" ]]; then
    echo "[warn] VERIFY_HASH=1 but PI_SUDO_PASSWORD is unset; skipping live hash verification" >&2
  else
    echo "[5/5] verifying live worker binary hash"
    EXEC_ID="hash$(date +%s)"
    LIVE_HASH="$(printf '%s\n' "${PI_SUDO_PASSWORD}" | ssh "${PI_SSH}" "sudo -S -p '' ctr -n openfaas-fn task exec --exec-id '${EXEC_ID}' '${FUNCTION_A}' /bin/sh -lc 'sha256sum /usr/local/bin/bench2_proto_worker | cut -d\" \" -f1'" | tail -n 1)"
    echo "[remote] live hash: ${LIVE_HASH}"

    if [[ "${LIVE_HASH}" != "${EXPECTED_HASH}" ]]; then
      echo "ERROR: live worker hash does not match expected image hash" >&2
      exit 1
    fi
  fi
fi

echo
echo "IMAGE_REF=${IMAGE_REF}"
echo "EXPECTED_HASH=${EXPECTED_HASH}"
echo "[ok] prototype worker deployed"