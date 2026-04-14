#!/usr/bin/env bash
# Copies the patched faasd binary to the Pi and restarts faasd-provider.
# Requires sudo on the Pi (will prompt).
set -euo pipefail

PI_SSH="${PI_SSH:-romero@192.168.2.2}"
BIN_LOCAL="${BIN_LOCAL:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/dist/faasd.tlsmigrate.linux-arm64}"

if [ ! -f "$BIN_LOCAL" ]; then
  echo "ERROR: binary not found: $BIN_LOCAL" >&2
  echo "Build it first: prototype/faasd_tlsmigrate/provider/build_faasd_arm64.sh" >&2
  exit 1
fi

echo "[scp] $BIN_LOCAL -> $PI_SSH:/tmp/faasd.tlsmigrate"
scp "$BIN_LOCAL" "$PI_SSH:/tmp/faasd.tlsmigrate"

echo "[ssh] installing patched /usr/local/bin/faasd and restarting faasd-provider"
ssh -t "$PI_SSH" '
set -e
if [ ! -f /usr/local/bin/faasd.vanilla ]; then
  echo "[pi] backing up /usr/local/bin/faasd -> /usr/local/bin/faasd.vanilla"
  sudo cp /usr/local/bin/faasd /usr/local/bin/faasd.vanilla
fi
sudo install -m 0755 /tmp/faasd.tlsmigrate /usr/local/bin/faasd
sudo systemctl restart faasd-provider
sudo systemctl status faasd-provider --no-pager --full | head -60
'

echo "[OK] prototype faasd-provider installed"
