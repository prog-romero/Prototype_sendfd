#!/usr/bin/env bash
#
# build-all.sh — construit et pousse les images de la fonction "sum" en Python
# et JS, mode vanilla et/ou full-proxy (prototype), pour arm64 (Pi).
#
# DOIT être lancé depuis la RACINE du dépôt (le contexte de build = racine) :
#   cd <repo-root>
#   ./benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/build-all.sh [all|python|js|vanilla|proto]
#
# Filtre (argument, def: all) :
#   all      -> les 4 images
#   python   -> sum-python-{vanilla,fullproxy}
#   js       -> sum-js-{vanilla,fullproxy}
#   vanilla  -> sum-{python,js}-vanilla
#   proto    -> sum-{python,js}-fullproxy
#
# Variables :
#   REGISTRY   préfixe d'image Docker Hub (def: romerosdd)
#   PLATFORM   plateforme cible          (def: linux/arm64)
#   PUSH       1 = --push, 0 = --load    (def: 1)
#
set -euo pipefail

FILTER="${1:-all}"
REGISTRY="${REGISTRY:-romerosdd}"
PLATFORM="${PLATFORM:-linux/arm64}"
PUSH="${PUSH:-1}"

DIR="benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function"

if [[ ! -f "$DIR/sum-python/Dockerfile.vanilla" ]]; then
  echo "ERREUR: lancez ce script depuis la RACINE du dépôt." >&2
  exit 1
fi

push_flag="--push"
[[ "$PUSH" == "0" ]] && push_flag="--load"

# runtime kind image_suffix dockerfile_suffix
build_one() {
  local runtime="$1" kind="$2"      # kind = vanilla | fullproxy
  local tag="$REGISTRY/sum-${runtime}-${kind}:latest"
  local dockerfile="$DIR/sum-${runtime}/Dockerfile.${kind}"
  echo ">>> build [$runtime/$kind] -> $tag"
  docker buildx build \
    --platform "$PLATFORM" \
    -f "$dockerfile" \
    -t "$tag" \
    $push_flag \
    .
}

# Sélection selon le filtre.
declare -a JOBS   # "runtime kind"
case "$FILTER" in
  all)     JOBS=("python vanilla" "python fullproxy" "js vanilla" "js fullproxy") ;;
  python)  JOBS=("python vanilla" "python fullproxy") ;;
  js)      JOBS=("js vanilla" "js fullproxy") ;;
  vanilla) JOBS=("python vanilla" "js vanilla") ;;
  proto)   JOBS=("python fullproxy" "js fullproxy") ;;
  *) echo "usage: $0 [all|python|js|vanilla|proto]" >&2; exit 1 ;;
esac

for job in "${JOBS[@]}"; do
  build_one $job
done

echo "=== build-all terminé (filtre: $FILTER) ==="
