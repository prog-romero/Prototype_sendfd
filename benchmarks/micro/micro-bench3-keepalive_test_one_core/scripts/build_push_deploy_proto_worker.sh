#!/usr/bin/env bash
# build_push_deploy_proto_worker.sh
#
# Builds the bench3 keepalive prototype worker image for linux/arm64, pushes it to ttl.sh
# under a unique tag, deploys bench2-fn-a / bench2-fn-b on the Pi, and can
# verify that the running worker binary matches the pushed image.
#
# Environment variables:
#   PI_SSH              SSH target                      (default: romero@192.168.2.2)
#   BUILDER_NAME        buildx builder                 (default: bench2-arm64-builder)
#   BUILD_PROGRESS      plain|auto                     (default: auto)
#   BENCH2_DEBUG        1 keeps verbose tracing on     (default: 0)
#   CACHE_DIR           local buildx cache dir
#                        (default: <repo>/.buildx-cache/faasd-gateway)
#   USE_LOCAL_CACHE     auto|0|1                       (default: 1)
#   REQUIRE_LOCAL_CACHE 1 refuses cold builds when cache is missing (default: 1)
#   IMAGE_NAME          ttl.sh base name               (default: bench3-keepalive-proto-worker)
#   IMAGE_REF           full image ref                 (default: generated unique ttl.sh tag)
#   FUNCTION_A          first OpenFaaS function name   (default: bench2-fn-a)
#   FUNCTION_B          second OpenFaaS function name  (default: bench2-fn-b)
#   GATEWAY_URL         OpenFaaS gateway URL           (default: http://127.0.0.1:8080)
#   VERIFY_HASH         1 to verify live binary hash   (default: 1)
#   PI_SUDO_PASSWORD    sudo password for hash verify  (optional, required for VERIFY_HASH=1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"
DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench3-keepalive_test_one_core/proto_worker/Dockerfile"

: "${PI_SSH:=romero@192.168.2.2}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${BENCH2_DEBUG:=0}"
: "${CACHE_DIR:=${REPO_ROOT}/.buildx-cache/faasd-gateway}"
: "${USE_LOCAL_CACHE:=1}"
: "${REQUIRE_LOCAL_CACHE:=1}"
: "${IMAGE_NAME:=bench3-keepalive-proto-worker}"
: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${GATEWAY_URL:=http://127.0.0.1:8080}"
: "${VERIFY_HASH:=1}"

builder_has_cache() {
  docker buildx du --builder "${BUILDER_NAME}" 2>/dev/null | awk 'NR == 2 { found = 1 } END { exit(found ? 0 : 1) }'
}

promote_tmp_cache() {
  local tmp_cache
  tmp_cache="${CACHE_DIR}.tmp"
  if [[ -d "${tmp_cache}" ]]; then
    rm -rf "${CACHE_DIR}"
    mv "${tmp_cache}" "${CACHE_DIR}"
  fi
}

prime_local_cache_from_gateway() {
  local gateway_dockerfile
  gateway_dockerfile="${REPO_ROOT}/benchmarks/micro/micro-bench3-keepalive/proto_gateway/Dockerfile"

  echo "[build] priming local cache from ${gateway_dockerfile} via ${BUILDER_NAME}"

  local prime_cmd=(docker buildx build)
  prime_cmd+=(--builder "${BUILDER_NAME}")
  prime_cmd+=(--platform linux/arm64)
  prime_cmd+=(--progress "${BUILD_PROGRESS}")
  prime_cmd+=(--build-arg "BENCH2_DEBUG=${BENCH2_DEBUG}")
  if [[ -f "${CACHE_DIR}/index.json" ]]; then
    prime_cmd+=(--cache-from "type=local,src=${CACHE_DIR}")
  fi
  prime_cmd+=(--cache-to "type=local,dest=${CACHE_DIR}.tmp,mode=max")
  prime_cmd+=(-f "${gateway_dockerfile}")
  prime_cmd+=(--target build)
  prime_cmd+=(--output type=cacheonly)
  prime_cmd+=("${REPO_ROOT}")

  "${prime_cmd[@]}"
  promote_tmp_cache
}

cleanup_cache() {
  promote_tmp_cache
}

trap cleanup_cache EXIT

mkdir -p "${CACHE_DIR}"

if [[ -z "${IMAGE_REF:-}" ]]; then
  IMAGE_REF="ttl.sh/${IMAGE_NAME}-$(date +%Y%m%d%H%M%S):24h"
fi

echo "[1/5] ensuring buildx builder: ${BUILDER_NAME}"
if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
  docker buildx create --name "${BUILDER_NAME}" --platform linux/arm64 --use
else
  docker buildx use "${BUILDER_NAME}"
fi

if ! BUILDER_INSPECT="$(docker buildx inspect "${BUILDER_NAME}" 2>&1)"; then
  echo "ERROR: failed to inspect buildx builder: ${BUILDER_NAME}" >&2
  printf '%s\n' "${BUILDER_INSPECT}" >&2
  docker buildx ls >&2 || true
  exit 1
fi

BUILDER_DRIVER="$(printf '%s\n' "${BUILDER_INSPECT}" | sed -n 's/^Driver:[[:space:]]*//p' | head -n 1 | tr -d '[:space:]')"
if [[ -z "${BUILDER_DRIVER}" ]]; then
  echo "ERROR: could not inspect buildx builder: ${BUILDER_NAME}" >&2
  printf '%s\n' "${BUILDER_INSPECT}" >&2
  exit 1
fi

case "${USE_LOCAL_CACHE}" in
  auto)
    if [[ "${BUILDER_DRIVER}" == "docker" ]]; then
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

case "${REQUIRE_LOCAL_CACHE}" in
  0|1)
    ;;
  *)
    echo "ERROR: REQUIRE_LOCAL_CACHE must be 0 or 1" >&2
    exit 1
    ;;
esac

if [[ "${USE_LOCAL_CACHE}" == "1" && "${BUILDER_DRIVER}" == "docker" ]]; then
  echo "ERROR: builder driver '${BUILDER_DRIVER}' cannot be used with USE_LOCAL_CACHE=1" >&2
  echo "       Use the docker-container builder '${BUILDER_NAME}' or set USE_LOCAL_CACHE=0 explicitly." >&2
  exit 1
fi

echo "[2/5] building and pushing ${IMAGE_REF}"
echo "[build] builder=${BUILDER_NAME} driver=${BUILDER_DRIVER} local_cache=${USE_LOCAL_CACHE} require_local_cache=${REQUIRE_LOCAL_CACHE} cache_dir=${CACHE_DIR}"

build_cmd=(docker buildx build)
build_cmd+=(--builder "${BUILDER_NAME}")
build_cmd+=(--platform linux/arm64)
build_cmd+=(--progress "${BUILD_PROGRESS}")
build_cmd+=(--build-arg "BENCH2_DEBUG=${BENCH2_DEBUG}")
if [[ "${USE_LOCAL_CACHE}" == "1" ]]; then
  if [[ -f "${CACHE_DIR}/index.json" ]]; then
    build_cmd+=(--cache-from "type=local,src=${CACHE_DIR}")
  elif builder_has_cache; then
    echo "[build] local cache metadata missing at ${CACHE_DIR}; reusing persistent builder cache from ${BUILDER_NAME} and refreshing local export"
    prime_local_cache_from_gateway
    if [[ -f "${CACHE_DIR}/index.json" ]]; then
      build_cmd+=(--cache-from "type=local,src=${CACHE_DIR}")
    fi
  elif [[ "${REQUIRE_LOCAL_CACHE}" == "1" ]]; then
    echo "ERROR: required local cache is missing at ${CACHE_DIR}" >&2
    echo "       Refusing to rebuild wolfSSL/libtlspeek without local or builder cache." >&2
    echo "       Set REQUIRE_LOCAL_CACHE=0 to allow a cold build." >&2
    exit 1
  else
    echo "[warn] local cache metadata missing at ${CACHE_DIR}; continuing because REQUIRE_LOCAL_CACHE=0" >&2
  fi
  build_cmd+=(--cache-to "type=local,dest=${CACHE_DIR}.tmp,mode=max")
fi
build_cmd+=(-f "${DOCKERFILE}")
build_cmd+=(-t "${IMAGE_REF}")
build_cmd+=(--push)
build_cmd+=("${REPO_ROOT}")

"${build_cmd[@]}"

echo "[3/5] computing expected worker binary hash from cached build output"
HASH_EXPORT_DIR="$(mktemp -d)"
trap 'cleanup_cache; rm -rf "${HASH_EXPORT_DIR}"' EXIT

hash_cmd=(docker buildx build)
hash_cmd+=(--builder "${BUILDER_NAME}")
hash_cmd+=(--platform linux/arm64)
hash_cmd+=(--progress "${BUILD_PROGRESS}")
hash_cmd+=(--build-arg "BENCH2_DEBUG=${BENCH2_DEBUG}")
if [[ "${USE_LOCAL_CACHE}" == "1" && -f "${CACHE_DIR}/index.json" ]]; then
  hash_cmd+=(--cache-from "type=local,src=${CACHE_DIR}")
fi
hash_cmd+=(-f "${DOCKERFILE}")
hash_cmd+=(--target hash-export)
hash_cmd+=(--output "type=local,dest=${HASH_EXPORT_DIR}")
hash_cmd+=("${REPO_ROOT}")

"${hash_cmd[@]}"

EXPECTED_HASH="$(sha256sum "${HASH_EXPORT_DIR}/bench2_proto_worker" | cut -d" " -f1)"
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