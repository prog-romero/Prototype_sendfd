#!/usr/bin/env bash
# pi_restore_vanilla_gw.sh
#
# Run ON THE PI to restore the faasd vanilla gateway from the backup created
# by pi_enable_proto_gw.sh.
# Optional env:
#   PI_SUDO_PASSWORD      — sudo password for non-interactive runs

set -euo pipefail

if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "${PI_SUDO_PASSWORD}" | sudo -S -p '' -v
fi

COMPOSE="/var/lib/faasd/docker-compose.yaml"
BACKUP="${COMPOSE}.bench2-vanilla-backup"

if [[ ! -f "${BACKUP}" ]]; then
    echo "[pi-restore] no backup found at ${BACKUP} — already vanilla?"
    exit 0
fi

echo "[pi-restore] restoring ${COMPOSE} from backup"
sudo cp "${BACKUP}" "${COMPOSE}"

echo "[pi-restore] restarting faasd"
sudo systemctl restart faasd
echo "[pi-restore] done — vanilla gateway restored"
