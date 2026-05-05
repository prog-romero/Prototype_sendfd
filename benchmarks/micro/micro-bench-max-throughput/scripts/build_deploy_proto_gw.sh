#!/usr/bin/env bash
# build_deploy_proto_gw.sh
#
# Builds the bench3 keepalive gateway image for linux/arm64 on the laptop,
# exports it as a tar archive, SCPs it to the Pi, imports it into containerd
# (openfaas namespace), then optionally calls a gateway-enable helper on the Pi.
#
# Environment variables (all optional):
#   PI_SSH              SSH target      (default: romero@192.168.2.2)
#   IMAGE_REF           image ref       (default: docker.io/local/bench3-keepalive-gateway:arm64)
#   LOCAL_ARCHIVE       local tar path  (default: /tmp/bench3_keepalive_gateway.tar)
#   REMOTE_ARCHIVE      remote tar path (default: /tmp/bench3_keepalive_gateway.tar)
#   ENABLE_ON_PI        set to "1" to call pi_enable_proto_gw.sh after import
#   BUILDER_NAME        buildx builder  (default: bench2-arm64-builder)
#   BUILD_PROGRESS      plain|auto      (default: auto)
#   CACHE_DIR           local buildx cache dir
#                        (default: <repo>/.buildx-cache/faasd-gateway)
#   USE_LOCAL_CACHE     auto|0|1        (default: 1)
#   REQUIRE_LOCAL_CACHE 1 refuses cold builds when cache is missing (default: 1)
#   BENCH2_DEBUG        1 to keep verbose libtlspeek logs enabled (default: 0)
#   ENABLE_HELPER       Pi-side helper  (default: pi_enable_proto_gw.sh)
#   PI_SUDO_PASSWORD    sudo password on the Pi (optional)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"

: "${PI_SSH:=romero@192.168.2.2}"
: "${IMAGE_REF:=docker.io/local/bench3-keepalive-gateway:arm64}"
: "${LOCAL_ARCHIVE:=/tmp/bench3_keepalive_gateway.tar}"
: "${REMOTE_ARCHIVE:=/tmp/bench3_keepalive_gateway.tar}"
: "${ENABLE_ON_PI:=0}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${CACHE_DIR:=${REPO_ROOT}/.buildx-cache/faasd-gateway}"
: "${USE_LOCAL_CACHE:=1}"
: "${REQUIRE_LOCAL_CACHE:=1}"
: "${BENCH2_DEBUG:=0}"
: "${ENABLE_HELPER:=pi_enable_proto_gw.sh}"

if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
    PI_SUDO_CMD="sudo -S -p ''"
else
    PI_SUDO_CMD="sudo"
fi

run_remote() {
    local script_text="$1"
    if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
        printf '%s\n' "${PI_SUDO_PASSWORD}" | ssh "${PI_SSH}" "${script_text}"
    else
        ssh "${PI_SSH}" "${script_text}"
    fi
}

builder_has_cache() {
    docker buildx du --builder "${BUILDER_NAME}" 2>/dev/null | awk 'NR == 2 { found = 1 } END { exit(found ? 0 : 1) }'
}

DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench3-keepalive/proto_gateway/Dockerfile"

cleanup_cache() {
    local tmp_cache
    tmp_cache="${CACHE_DIR}.tmp"
    if [[ -d "${tmp_cache}" ]]; then
        rm -rf "${CACHE_DIR}"
        mv "${tmp_cache}" "${CACHE_DIR}"
    fi
}

trap cleanup_cache EXIT

mkdir -p "$(dirname "${LOCAL_ARCHIVE}")" "${CACHE_DIR}"

echo "=== [1/4] Creating/using arm64 buildx builder: ${BUILDER_NAME} ==="
if ! docker buildx inspect "${BUILDER_NAME}" &>/dev/null; then
    docker buildx create \
        --name "${BUILDER_NAME}" \
        --platform linux/arm64 \
        --use
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

echo "=== [2/4] Building ${IMAGE_REF} for linux/arm64 ==="
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
build_cmd+=("${REPO_ROOT}")

rm -f "${LOCAL_ARCHIVE}"
if [[ "${BUILDER_DRIVER}" == "docker" ]]; then
    echo "[build] docker driver detected; building into local image store first"
    "${build_cmd[@]}"
    docker image save -o "${LOCAL_ARCHIVE}" "${IMAGE_REF}"
else
    build_cmd+=(--output "type=docker,dest=${LOCAL_ARCHIVE}")
    "${build_cmd[@]}"
fi

echo "=== [3/4] Copying archive to Pi (${PI_SSH}:${REMOTE_ARCHIVE}) ==="
scp "${LOCAL_ARCHIVE}" "${PI_SSH}:${REMOTE_ARCHIVE}"

echo "=== [4/4] Importing image into containerd on Pi ==="
run_remote "
    set -euo pipefail
    ${PI_SUDO_CMD} ctr -n openfaas images import '${REMOTE_ARCHIVE}'
    echo '[pi] import done'
    rm -f '${REMOTE_ARCHIVE}'
"

if [[ "${ENABLE_ON_PI}" == "1" ]]; then
    echo "=== Enabling gateway mode on Pi via ${ENABLE_HELPER} ==="
    PI_SCRIPT="${SCRIPT_DIR}/${ENABLE_HELPER}"
    PI_REMOTE_SCRIPT="/tmp/${ENABLE_HELPER##*/}"
    scp "${PI_SCRIPT}" "${PI_SSH}:${PI_REMOTE_SCRIPT}"
    run_remote "
        GATEWAY_IMAGE='${IMAGE_REF}' PI_SUDO_PASSWORD='${PI_SUDO_PASSWORD:-}' bash '${PI_REMOTE_SCRIPT}'
        rm -f '${PI_REMOTE_SCRIPT}'
    "
fi

echo "=== Done. Image ${IMAGE_REF} is available on the Pi. ==="
