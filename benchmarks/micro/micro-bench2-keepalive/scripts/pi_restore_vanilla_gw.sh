#!/usr/bin/env bash
# pi_restore_vanilla_gw.sh
#
# Run ON THE PI to restore the faasd vanilla gateway from the backup created
# by pi_enable_proto_gw.sh.

set -euo pipefail

COMPOSE="/var/lib/faasd/docker-compose.yaml"
BACKUP="${COMPOSE}.bench2-vanilla-backup"

if [[ ! -f "${BACKUP}" ]]; then
    echo "[pi-restore] no backup found at ${BACKUP} — already vanilla?" >&2
    exit 1
fi

echo "[pi-restore] restoring ${COMPOSE} from backup"
sudo cp "${BACKUP}" "${COMPOSE}"

echo "[pi-restore] restarting faasd"
sudo systemctl restart faasd
echo "[pi-restore] done — vanilla gateway restored"
