#!/usr/bin/env bash
#
# build-all.sh — construit et pousse les images des 4 fonctions SeBS, en mode
# vanilla et/ou full-proxy (prototype), pour arm64 (Pi).
#
# DOIT être lancé depuis la RACINE du dépôt (le contexte de build = racine) :
#   cd <repo-root>
#   ./benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/build-all.sh [vanilla|proto|all] [fn...]
#
# Exemples :
#   ./.../sebs-bench/build-all.sh proto                 # les 4 fonctions, proto
#   ./.../sebs-bench/build-all.sh vanilla dynamic-html  # une seule fonction
#
# Variables :
#   REGISTRY   préfixe d'image Docker Hub (def: romerosdd)
#   PLATFORM   plateforme cible          (def: linux/arm64)
#   PUSH       1 = --push, 0 = --load    (def: 1)
#
set -euo pipefail

KIND="${1:-all}"
shift || true
REGISTRY="${REGISTRY:-romerosdd}"
PLATFORM="${PLATFORM:-linux/arm64}"
PUSH="${PUSH:-1}"

DIR="benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench"
ALL_FUNCTIONS=(dynamic-html thumbnailer compression graph-pagerank)
# fonctions ciblées = arguments restants, sinon les 4
if [[ "$#" -gt 0 ]]; then FUNCTIONS=("$@"); else FUNCTIONS=("${ALL_FUNCTIONS[@]}"); fi

if [[ ! -f "$DIR/Dockerfile.vanilla" ]]; then
  echo "ERREUR: lancez ce script depuis la RACINE du dépôt." >&2
  exit 1
fi

push_flag="--push"
[[ "$PUSH" == "0" ]] && push_flag="--load"

build_one() {
  local kind="$1" fn="$2" dockerfile tag
  if [[ "$kind" == "vanilla" ]]; then
    dockerfile="$DIR/Dockerfile.vanilla"
    tag="$REGISTRY/sebs-vanilla-$fn:latest"
  else
    dockerfile="$DIR/Dockerfile.fullproxy"
    tag="$REGISTRY/sebs-fullproxy-$fn:latest"
  fi
  echo ">>> build [$kind] $fn -> $tag"
  docker buildx build \
    --platform "$PLATFORM" \
    --build-arg "FUNCTION_NAME=$fn" \
    -f "$dockerfile" \
    -t "$tag" \
    $push_flag \
    .
}

do_kind() {
  local kind="$1"
  for fn in "${FUNCTIONS[@]}"; do
    build_one "$kind" "$fn"
  done
}

case "$KIND" in
  vanilla) do_kind vanilla ;;
  proto)   do_kind proto ;;
  all)     do_kind vanilla; do_kind proto ;;
  *) echo "usage: $0 [vanilla|proto|all] [fn...]" >&2; exit 1 ;;
esac

echo "=== build-all terminé ($KIND : ${FUNCTIONS[*]}) ==="
