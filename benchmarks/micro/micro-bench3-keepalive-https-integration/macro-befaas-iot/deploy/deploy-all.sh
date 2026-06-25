#!/usr/bin/env bash
#
# deploy-all.sh — déploie les 4 fonctions IoT BeFaaS sur faasd (à lancer SUR LE PI).
#
# Gère la matrice d'évaluation : KIND (vanilla|proto) × MODE (http|https).
# Il part du stack correspondant et réécrit OPENFAAS_ENDPOINT (et le port du
# gateway) selon MODE, puis déploie via faas-cli.
#
#   ./deploy-all.sh <vanilla|proto> <http|https>
#
# Variables :
#   GATEWAY_IP   IP du gateway joignable depuis un conteneur (def: 192.168.2.2)
#   REDIS_IP     IP du conteneur Redis                        (def: 10.62.0.5)
#   GATEWAY_URL  URL d'admin faas-cli                         (def: http://127.0.0.1:8080)
#
set -euo pipefail

KIND="${1:?usage: $0 <vanilla|proto> <http|https>}"
MODE="${2:?usage: $0 <vanilla|proto> <http|https>}"

GATEWAY_IP="${GATEWAY_IP:-192.168.2.2}"
# IP du bridge faasd openfaas0 : stable, c'est par là que les fonctions joignent
# aussi le gateway (10.62.0.1:8080). Redis est publié sur cette IP (Option B).
REDIS_IP="${REDIS_IP:-10.62.0.1}"
GATEWAY_URL="${GATEWAY_URL:-http://127.0.0.1:8080}"

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$KIND" in
  vanilla) SRC="$SELF/stack-vanilla.yml" ;;
  proto)   SRC="$SELF/stack-proto.yml" ;;
  *) echo "KIND invalide: $KIND" >&2; exit 1 ;;
esac

case "$MODE" in
  http)  ENDPOINT="http://${GATEWAY_IP}:8080/function" ;;
  https) ENDPOINT="https://${GATEWAY_IP}:8443/function" ;;
  *) echo "MODE invalide: $MODE" >&2; exit 1 ;;
esac

TMP="$(mktemp /tmp/iot-stack-${KIND}-${MODE}-XXXX.yml)"
# Réécrit OPENFAAS_ENDPOINT, REDIS_ENDPOINT et l'URL d'admin.
sed -E \
  -e "s#OPENFAAS_ENDPOINT: \".*\"#OPENFAAS_ENDPOINT: \"${ENDPOINT}\"#" \
  -e "s#REDIS_ENDPOINT: \".*\"#REDIS_ENDPOINT: \"redis://${REDIS_IP}:6379\"#" \
  -e "s#gateway: http://127.0.0.1:8080#gateway: ${GATEWAY_URL}#" \
  "$SRC" > "$TMP"

echo ">>> Déploiement KIND=$KIND MODE=$MODE"
echo "    OPENFAAS_ENDPOINT = $ENDPOINT"
echo "    REDIS_ENDPOINT    = redis://${REDIS_IP}:6379"
echo "    stack rendu       = $TMP"
echo

faas-cli deploy -f "$TMP" --gateway "$GATEWAY_URL"

echo "=== déploiement terminé. Vérifiez : faas-cli list --gateway $GATEWAY_URL ==="
