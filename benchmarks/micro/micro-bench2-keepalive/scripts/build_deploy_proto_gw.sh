#!/usr/bin/env bash
# build_deploy_proto_gw.sh
#
# Builds the bench2 prototype gateway image for linux/arm64 on the laptop,
# exports it as a tar archive, SCPs it to the Pi, imports it into containerd
# (openfaas namespace), then optionally calls pi_enable_proto_gw.sh on the Pi.
#
# Environment variables (all optional):
#   PI_SSH              SSH target      (default: romero@192.168.2.2)
#   IMAGE_REF           image ref       (default: docker.io/local/bench2-proto-gateway:arm64)
#   LOCAL_ARCHIVE       local tar path  (default: /tmp/bench2_proto_gateway.tar)
#   REMOTE_ARCHIVE      remote tar path (default: /tmp/bench2_proto_gateway.tar)
#   ENABLE_ON_PI        set to "1" to call pi_enable_proto_gw.sh after import
#   BUILDER_NAME        buildx builder  (default: bench2-arm64-builder)
#   BUILD_PROGRESS      plain|auto      (default: auto)
#   BENCH2_DEBUG        1 to keep verbose libtlspeek logs enabled (default: 0)
#   PI_SUDO_PASSWORD    sudo password on the Pi (optional)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../../"

: "${PI_SSH:=romero@192.168.2.2}"
: "${IMAGE_REF:=docker.io/local/bench2-proto-gateway:arm64}"
: "${LOCAL_ARCHIVE:=/tmp/bench2_proto_gateway.tar}"
: "${REMOTE_ARCHIVE:=/tmp/bench2_proto_gateway.tar}"
: "${ENABLE_ON_PI:=0}"
: "${BUILDER_NAME:=bench2-arm64-builder}"
: "${BUILD_PROGRESS:=auto}"
: "${BENCH2_DEBUG:=0}"

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

DOCKERFILE="${REPO_ROOT}/benchmarks/micro/micro-bench2-keepalive/proto_gateway/Dockerfile"

echo "=== [1/4] Creating/using arm64 buildx builder: ${BUILDER_NAME} ==="
if ! docker buildx inspect "${BUILDER_NAME}" &>/dev/null; then
    docker buildx create \
        --name "${BUILDER_NAME}" \
        --platform linux/arm64 \
        --use
else
    docker buildx use "${BUILDER_NAME}"
fi

echo "=== [2/4] Building ${IMAGE_REF} for linux/arm64 ==="
docker buildx build \
    --platform linux/arm64 \
    --progress "${BUILD_PROGRESS}" \
    --build-arg "BENCH2_DEBUG=${BENCH2_DEBUG}" \
    -f "${DOCKERFILE}" \
    -t "${IMAGE_REF}" \
    --output "type=docker,dest=${LOCAL_ARCHIVE}" \
    "${REPO_ROOT}"

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
    echo "=== Enabling proto gateway on Pi ==="
    PI_SCRIPT="${SCRIPT_DIR}/pi_enable_proto_gw.sh"
    scp "${PI_SCRIPT}" "${PI_SSH}:/tmp/pi_enable_proto_gw.sh"
    run_remote "
        PROTO_GATEWAY_IMAGE='${IMAGE_REF}' PI_SUDO_PASSWORD='${PI_SUDO_PASSWORD:-}' bash /tmp/pi_enable_proto_gw.sh
        rm -f /tmp/pi_enable_proto_gw.sh
    "
fi

echo "=== Done. Image ${IMAGE_REF} is available on the Pi. ==="
