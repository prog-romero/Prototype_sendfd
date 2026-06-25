#!/usr/bin/env bash
#
# run_eval.sh — lance le rate-sweep wrk2 du macro-bench BeFaaS IoT et range le
# CSV dans results/ avec un nom normalisé.
#
#   ./run_eval.sh <vanilla|proto> <http|https> [rates] [concurrency]
#
# Exemple :
#   ./run_eval.sh proto   https
#   ./run_eval.sh vanilla https 25,50,75,100,150,200 100
#
# Variables :
#   GATEWAY_IP   IP du gateway       (def 192.168.2.2)
#   PI_SSH       SSH du Pi           (def romero@192.168.2.2)
#   IMAGE        image multipart     (def images/image-ambulance.jpg)
#   DURATION     durée par palier s  (def 20)
#
set -euo pipefail

MODE="${1:?usage: $0 <vanilla|proto> <http|https> [rates] [concurrency]}"
SCHEME="${2:?usage: $0 <vanilla|proto> <http|https> [rates] [concurrency]}"
RATES="${3:-5,10,15,20,25,30,40,50}"
CONC="${4:-8}"

GATEWAY_IP="${GATEWAY_IP:-192.168.2.2}"
PI_SSH="${PI_SSH:-romero@192.168.2.2}"
IMAGE="${IMAGE:-images/image-ambulance.jpg}"
DURATION="${DURATION:-20}"

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SELF/results/${MODE}_${SCHEME}_objreco_${CONC}c.csv"

echo ">>> Sweep MODE=$MODE SCHEME=$SCHEME rates=$RATES conc=$CONC"
python3 "$SELF/sweep_app_wrk2.py" \
  --mode "$MODE" --scheme "$SCHEME" \
  --gateway-ip "$GATEWAY_IP" --pi-ssh "$PI_SSH" \
  --rates "$RATES" --concurrency "$CONC" \
  --image "$IMAGE" --duration-s "$DURATION" \
  --out "$OUT"

echo "=== CSV: $OUT ==="
