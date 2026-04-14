#!/usr/bin/env bash
# Restore the vanilla faasd docker-compose.yaml (gateway image/ports/volumes/env) from backup.
# Run this ON THE PI.
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"

if [ ! -f "$BACKUP_FILE" ]; then
  echo "ERROR: backup file not found: $BACKUP_FILE" >&2
  echo "Nothing to restore." >&2
  exit 1
fi

echo "[pi] restoring $COMPOSE_FILE from $BACKUP_FILE"
sudo cp "$BACKUP_FILE" "$COMPOSE_FILE"

echo "[pi] restarting faasd (gateway container)"
sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[OK] vanilla gateway restored"
