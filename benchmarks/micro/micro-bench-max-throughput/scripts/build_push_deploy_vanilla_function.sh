#!/usr/bin/env bash
# build_push_deploy_vanilla_function.sh
#
# Builds the bench3 keepalive vanilla function image for linux/arm64, pushes it to ttl.sh
# under a unique tag, and deploys bench2-fn-a / bench2-fn-b on the Pi through
# the vanilla OpenFaaS gateway.
#
# Environment variables:
#   PI_SSH              SSH target                      (default: romero@192.168.2.2)
#   BUILDER_NAME        buildx builder                 (default: bench2-arm64-builder)
#   BUILD_PROGRESS      plain|auto                     (default: auto)
#   CACHE_DIR           local buildx cache dir
#                        (default: <repo>/.buildx-cache/faasd-gateway)
#   USE_LOCAL_CACHE     auto|0|1                       (default: 1)
#   REQUIRE_LOCAL_CACHE 1 refuses cold builds when cache is missing (default: 1)
#   IMAGE_NAME          ttl.sh base name               (default: bench3-keepalive-vanilla-fn)
#   IMAGE_REF           full image ref                 (default: generated unique ttl.sh tag)
#   FUNCTION_A          first OpenFaaS function name   (default: bench2-fn-a)
#   FUNCTION_B          second OpenFaaS function name  (default: bench2-fn-b)
#   GATEWAY_URL         OpenFaaS gateway URL           (default: http://127.0.0.1:8080)

set -euo pipefail

: "${NUM_CORES:=}"
CPU_SET="${CPU_SET:-}"
if [[ -n "${NUM_CORES}" ]]; then
    if [[ "${NUM_CORES}" -eq 1 ]]; then
        CPU_SET="0"
    else
        CPU_SET="0-$((NUM_CORES - 1))"
    fi
    echo "[build] CPU pinning requested: ${NUM_CORES} core(s) (set: ${CPU_SET})"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"
DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench3-keepalive/vanilla_function/Dockerfile"

: "${PI_SSH:=romero@192.168.2.2}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${CACHE_DIR:=${REPO_ROOT}/.buildx-cache/faasd-gateway}"
: "${USE_LOCAL_CACHE:=1}"
: "${REQUIRE_LOCAL_CACHE:=1}"
: "${IMAGE_NAME:=bench3-keepalive-vanilla-fn}"
: "${FUNCTION_A:=bench2-fn-a}"
: "${FUNCTION_B:=bench2-fn-b}"
: "${GATEWAY_URL:=http://127.0.0.1:8080}"

builder_has_cache() {
  docker buildx du --builder "${BUILDER_NAME}" 2>/dev/null | awk 'NR == 2 { found = 1 } END { exit(found ? 0 : 1) }'
}

cleanup_cache() {
  local tmp_cache
  tmp_cache="${CACHE_DIR}.tmp"
  if [[ -d "${tmp_cache}" ]]; then
    rm -rf "${CACHE_DIR}"
    mv "${tmp_cache}" "${CACHE_DIR}"
  fi
}

trap cleanup_cache EXIT

mkdir -p "${CACHE_DIR}"

if [[ -z "${IMAGE_REF:-}" ]]; then
  IMAGE_REF="ttl.sh/${IMAGE_NAME}-$(date +%Y%m%d%H%M%S):24h"
fi

echo "[1/3] ensuring buildx builder: ${BUILDER_NAME}"
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

echo "[2/3] building and pushing ${IMAGE_REF}"
echo "[build] builder=${BUILDER_NAME} driver=${BUILDER_DRIVER} local_cache=${USE_LOCAL_CACHE} require_local_cache=${REQUIRE_LOCAL_CACHE} cache_dir=${CACHE_DIR}"

build_cmd=(docker buildx build)
build_cmd+=(--builder "${BUILDER_NAME}")
build_cmd+=(--platform linux/arm64)
build_cmd+=(--progress "${BUILD_PROGRESS}")
if [[ "${USE_LOCAL_CACHE}" == "1" ]]; then
  if [[ -f "${CACHE_DIR}/index.json" ]]; then
    build_cmd+=(--cache-from "type=local,src=${CACHE_DIR}")
  elif builder_has_cache; then
    echo "[build] local cache metadata missing at ${CACHE_DIR}; reusing persistent builder cache from ${BUILDER_NAME} and refreshing local export"
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

echo "[3/3] deploying ${FUNCTION_A} and ${FUNCTION_B} on ${PI_SSH}"
ssh "${PI_SSH}" "
  set -euo pipefail
  fa-deploy() {
    local name=\"\$1\"
    faas-cli remove \"\$name\" --gateway '${GATEWAY_URL}' >/dev/null 2>&1 || true
    faas-cli deploy --image '${IMAGE_REF}' --name \"\$name\" --gateway '${GATEWAY_URL}' \\
      --env BENCH2_WORKER_NAME=\"\$name\" \\
      --env BENCH2_LISTEN_PORT=8080 \\
      --env CPU_SET='${CPU_SET:-}' \\
      --label com.openfaas.scale.zero=false
  }

  fa-deploy '${FUNCTION_A}'
  fa-deploy '${FUNCTION_B}'
  faas-cli describe '${FUNCTION_A}' --gateway '${GATEWAY_URL}'
"

echo
echo "IMAGE_REF=${IMAGE_REF}"
echo "[ok] vanilla functions deployed"