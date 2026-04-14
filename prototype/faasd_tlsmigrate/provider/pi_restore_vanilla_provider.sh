#!/usr/bin/env bash
# Restores the original faasd binary on the Pi (if backed up) and restarts faasd-provider.
# Requires sudo on the Pi (will prompt).
set -euo pipefail

PI_SSH="${PI_SSH:-romero@192.168.2.2}"

echo "[ssh] restoring /usr/local/bin/faasd from /usr/local/bin/faasd.vanilla"
ssh -t "$PI_SSH" '
set -e
if [ ! -f /usr/local/bin/faasd.vanilla ]; then
  echo "ERROR: /usr/local/bin/faasd.vanilla not found (no backup)" >&2
  exit 1
fi
sudo cp /usr/local/bin/faasd.vanilla /usr/local/bin/faasd
sudo systemctl restart faasd-provider
sudo systemctl status faasd-provider --no-pager --full | head -60
'

echo "[OK] vanilla faasd-provider restored"
