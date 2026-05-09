#!/usr/bin/env bash
# Enable the prototype HTTP benchmark gateway on the Pi.
# Mirrors exactly how micro-bench2-initale-time handles /run/tlsmigrate:
#   - host dir: /var/lib/faasd/tlsmigrate   (= ./tlsmigrate relative to faasd workdir)
#   - gateway mount: ./tlsmigrate -> /run/tlsmigrate
#   - worker sees the same /run/tlsmigrate because faasd-provider bind-mounts it too
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-}"

run_sudo() {
  if [ -n "$PI_SUDO_PASSWORD" ]; then
    printf '%s\n' "$PI_SUDO_PASSWORD" | sudo -S -p "" "$@"
  else
    sudo -n "$@" || {
      echo "ERROR: sudo requires a password. Set PI_SUDO_PASSWORD." >&2
      exit 1
    }
  fi
}

PROTO_GATEWAY_IMAGE="${PROTO_GATEWAY_IMAGE:-}"
if [ -z "$PROTO_GATEWAY_IMAGE" ]; then
  echo "ERROR: set PROTO_GATEWAY_IMAGE" >&2
  exit 1
fi

# Create the shared socket dir on the host (= /var/lib/faasd/tlsmigrate)
# faasd's working dir is /var/lib/faasd, so ./tlsmigrate resolves here.
echo "[setup] creating /var/lib/faasd/tlsmigrate"
run_sudo mkdir -p /var/lib/faasd/tlsmigrate
run_sudo chmod 777 /var/lib/faasd/tlsmigrate

# Back up the pristine vanilla compose (only once)
if ! run_sudo test -f "$BACKUP_FILE"; then
  echo "[setup] backing up $COMPOSE_FILE -> $BACKUP_FILE"
  run_sudo cp "$COMPOSE_FILE" "$BACKUP_FILE"
fi

# Always patch from the backup to avoid stacking corruption
echo "[patch] updating gateway config from backup"
TMP_IN="$(mktemp)"
TMP_OUT="$(mktemp)"
run_sudo cat "$BACKUP_FILE" > "$TMP_IN"

python3 - "$TMP_IN" "$PROTO_GATEWAY_IMAGE" "$TMP_OUT" <<'PY'
import sys, yaml

compose_file, proto_image, tmp_out = sys.argv[1:4]

with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f)

gw = doc['services']['gateway']
gw['image'] = proto_image

# --- Environment ---
env = gw.get('environment', {})
if isinstance(env, dict):
    env['HTTPMIGRATE_KA_ENABLE'] = '1'
    env['HTTPMIGRATE_KA_LISTEN'] = ':8083'
    env['HTTPMIGRATE_KA_SOCKET_DIR'] = '/run/tlsmigrate'
    env['HTTPMIGRATE_KA_RELAY_SOCKET'] = '/run/tlsmigrate/relay.sock'
else:
    env = [e for e in env if not e.startswith('HTTPMIGRATE_')]
    env.extend([
        'HTTPMIGRATE_KA_ENABLE=1',
        'HTTPMIGRATE_KA_LISTEN=:8083',
        'HTTPMIGRATE_KA_SOCKET_DIR=/run/tlsmigrate',
        'HTTPMIGRATE_KA_RELAY_SOCKET=/run/tlsmigrate/relay.sock',
    ])
    gw['environment'] = env

# --- Ports ---
ports = gw.get('ports', [])
if '8083:8083' not in ports:
    ports.append('8083:8083')
gw['ports'] = ports

# --- Volumes ---
# Use the RELATIVE path './tlsmigrate' (not absolute '/var/lib/faasd/tlsmigrate').
# faasd resolves './' relative to its working directory (/var/lib/faasd), so
# ./tlsmigrate -> /var/lib/faasd/tlsmigrate on the host.
# This also causes faasd-provider to bind-mount the same dir into function
# containers at /run/tlsmigrate, which is exactly what we want.
vols = gw.get('volumes', [])
# Remove any previous tlsmigrate mount
vols = [v for v in vols if 'tlsmigrate' not in str(v)]
# Ensure tlsmigrate is present
if not any('tlsmigrate' in str(v) for v in vols):
    vols.append({'type': 'bind', 'source': './tlsmigrate', 'target': '/run/tlsmigrate'})
gw['volumes'] = vols

with open(tmp_out, 'w') as f:
    yaml.safe_dump(doc, f, sort_keys=False)

print('[OK] compose updated — gateway will mount ./tlsmigrate:/run/tlsmigrate')
PY

echo "[apply] installing new compose"
run_sudo cp "$TMP_OUT" "$COMPOSE_FILE"
rm -f "$TMP_IN" "$TMP_OUT"

# Single clean restart
echo "[restart] restarting faasd"
run_sudo systemctl restart faasd
sleep 6

run_sudo systemctl is-active faasd && echo "[OK] faasd is active" || { echo "[FAIL] faasd not active" >&2; exit 1; }
echo "[OK] HTTP prototype gateway enabled — socket dir: /var/lib/faasd/tlsmigrate"