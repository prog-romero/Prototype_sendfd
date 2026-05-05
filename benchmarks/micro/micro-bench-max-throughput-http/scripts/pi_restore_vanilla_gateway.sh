#!/usr/bin/env bash
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"

if ! sudo test -f "$BACKUP_FILE"; then
  echo "ERROR: backup compose not found at $BACKUP_FILE" >&2
  exit 1
fi

echo "[pi] restoring $COMPOSE_FILE from $BACKUP_FILE"
sudo cp "$BACKUP_FILE" "$COMPOSE_FILE"
sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[OK] restored vanilla faasd gateway compose"
