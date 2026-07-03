#!/usr/bin/env bash
#
# deploy-all.sh — déploie les 4 fonctions SeBS sur faasd (à lancer SUR LE PI).
#
#   ./deploy-all.sh <vanilla|proto>
#
# Pas de paramètre http/https : contrairement à BeFaaS, ces fonctions ne font
# AUCUN appel inter-fonctions, donc rien à réécrire selon le schéma. UNE même
# image proto sert les évals HTTP (8080) ET HTTPS (8443) — le bridge dispatche
# selon le magic de la connexion migrée. Le schéma se choisit côté CLIENT à
# l'éval (port 8080 vs 8443).
#
# Variables :
#   MINIO_IP     IP MinIO joignable par les fonctions (def: 10.62.0.1)
#   GATEWAY_URL  URL d'admin faas-cli                 (def: http://127.0.0.1:8080)
#
# IMPORTANT : déploiement UNE FONCTION À LA FOIS (--filter) pour éviter la race
# de snapshots containerd de faasd (cf. macro-befaas-iot).
set -euo pipefail

KIND="${1:?usage: $0 <vanilla|proto>}"

MINIO_IP="${MINIO_IP:-10.62.0.1}"
GATEWAY_URL="${GATEWAY_URL:-http://127.0.0.1:8080}"

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$KIND" in
  vanilla) SRC="$SELF/stack-vanilla.yml" ;;
  proto)   SRC="$SELF/stack-proto.yml" ;;
  *) echo "KIND invalide: $KIND (vanilla|proto)" >&2; exit 1 ;;
esac

TMP="$(mktemp /tmp/sebs-stack-${KIND}-XXXX.yml)"
# Réécrit l'URL MinIO et l'URL d'admin gateway.
sed -E \
  -e "s#MINIO_STORAGE_CONNECTION_URL: \".*\"#MINIO_STORAGE_CONNECTION_URL: \"${MINIO_IP}:9000\"#" \
  -e "s#gateway: http://127.0.0.1:8080#gateway: ${GATEWAY_URL}#" \
  "$SRC" > "$TMP"

echo ">>> Déploiement SeBS KIND=$KIND"
echo "    MINIO = ${MINIO_IP}:9000"
echo "    stack = $TMP"
echo

FUNCTIONS="dynamic-html thumbnailer compression graph-pagerank"
rc=0
for fn in $FUNCTIONS; do
  echo ">>> deploy $fn"
  if ! faas-cli deploy -f "$TMP" --filter "$fn" --gateway "$GATEWAY_URL"; then
    rc=1
  fi
  sleep 3
done

echo
echo "=== déploiement terminé. Vérifiez : faas-cli list --gateway $GATEWAY_URL ==="
exit $rc
