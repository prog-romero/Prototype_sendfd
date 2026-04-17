#!/usr/bin/env bash
# Enable the prototype gateway image + expose 9443 + mount ./tlsmigrate -> /run/tlsmigrate.
# Run this ON THE PI.
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"

# You must set this to the image you built/pushed.
PROTO_GATEWAY_IMAGE="${PROTO_GATEWAY_IMAGE:-}"
if [ -z "$PROTO_GATEWAY_IMAGE" ]; then
  echo "ERROR: set PROTO_GATEWAY_IMAGE to your custom gateway image tag" >&2
  echo "Example: export PROTO_GATEWAY_IMAGE=ttl.sh/faasd-gateway-tlsmigrate:24h" >&2
    echo "Example: export PROTO_GATEWAY_IMAGE=docker.io/local/faasd-gateway-tlsmigrate:arm64" >&2
  exit 1
fi

sudo mkdir -p /var/lib/faasd/tlsmigrate
sudo chmod 777 /var/lib/faasd/tlsmigrate

if ! sudo test -f "$BACKUP_FILE"; then
  if sudo env COMPOSE_FILE="$COMPOSE_FILE" python3 - <<'PY'
import os
import sys
import yaml

compose_file = os.environ['COMPOSE_FILE']
with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f) or {}

gw = ((doc.get('services') or {}).get('gateway') or {})
image = str(gw.get('image') or '')
env = gw.get('environment') or []
ports = gw.get('ports') or []
vols = gw.get('volumes') or []

def has_tlsmigrate_env(seq):
    if isinstance(seq, dict):
        return any(k.startswith('TLSMIGRATE_') for k in seq)
    if isinstance(seq, list):
        return any(isinstance(x, str) and x.startswith('TLSMIGRATE_') for x in seq)
    return False

def has_9443(seq):
    if not isinstance(seq, list):
        return False
    return any(isinstance(x, str) and x.split(':', 1)[0] == '9443' for x in seq)

def has_tlsmigrate_mount(seq):
    if not isinstance(seq, list):
        return False
    for item in seq:
        if isinstance(item, dict) and item.get('source') == './tlsmigrate':
            return True
        if isinstance(item, str) and item.split(':', 1)[0] == './tlsmigrate':
            return True
    return False

is_proto = (
    'faasd-gateway-tlsmigrate' in image or
    has_tlsmigrate_env(env) or
    has_9443(ports) or
    has_tlsmigrate_mount(vols)
)

sys.exit(1 if is_proto else 0)
PY
  then
    echo "[pi] backing up $COMPOSE_FILE -> $BACKUP_FILE"
    sudo cp "$COMPOSE_FILE" "$BACKUP_FILE"
  else
    echo "[pi] current compose already looks prototype-modified; not overwriting $BACKUP_FILE"
  fi
fi

echo "[pi] patching gateway service in $COMPOSE_FILE"
sudo env COMPOSE_FILE="$COMPOSE_FILE" PROTO_GATEWAY_IMAGE="$PROTO_GATEWAY_IMAGE" python3 - <<'PY'
import os
import yaml

compose_file = os.environ.get('COMPOSE_FILE', '/var/lib/faasd/docker-compose.yaml')
proto_image = os.environ['PROTO_GATEWAY_IMAGE']

with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f)

services = doc.setdefault('services', {})
gw = services.get('gateway')
if not isinstance(gw, dict):
    raise SystemExit('ERROR: services.gateway not found or invalid')

gw['image'] = proto_image

# Ensure env vars exist
env = gw.get('environment')
if env is None:
    env = []
    gw['environment'] = env

# Support list-of-strings and dict formats
if isinstance(env, dict):
    env.setdefault('TLSMIGRATE_ENABLE', '1')
    env.setdefault('TLSMIGRATE_LISTEN', ':9443')
    env.setdefault('TLSMIGRATE_SOCKET_DIR', '/run/tlsmigrate')
    env.setdefault('TLSMIGRATE_CERT', '/certs/server.crt')
    env.setdefault('TLSMIGRATE_KEY', '/certs/server.key')
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')

    def ensure(kv: str):
        key = kv.split('=', 1)[0]
        if not any(isinstance(x, str) and x.split('=', 1)[0] == key for x in env):
            env.append(kv)

    ensure('TLSMIGRATE_ENABLE=1')
    ensure('TLSMIGRATE_LISTEN=:9443')
    ensure('TLSMIGRATE_SOCKET_DIR=/run/tlsmigrate')
    ensure('TLSMIGRATE_CERT=/certs/server.crt')
    ensure('TLSMIGRATE_KEY=/certs/server.key')

# Ensure ports includes 9443
ports = gw.get('ports')
if ports is None:
    ports = []
    gw['ports'] = ports
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')

if not any(isinstance(p, str) and p.split(':', 1)[0] == '9443' for p in ports):
    ports.append('9443:9443')

# Ensure volumes includes ./tlsmigrate bind mount
vols = gw.get('volumes')
if vols is None:
    vols = []
    gw['volumes'] = vols
if not isinstance(vols, list):
    raise SystemExit('ERROR: gateway.volumes must be a list')

wanted = {'type': 'bind', 'source': './tlsmigrate', 'target': '/run/tlsmigrate'}

def same_mount(v):
    if isinstance(v, dict):
        return v.get('type') == wanted['type'] and v.get('source') == wanted['source'] and v.get('target') == wanted['target']
    if isinstance(v, str):
        # string form: ./tlsmigrate:/run/tlsmigrate
        return v.split(':', 1)[0] == wanted['source'] and v.split(':', 1)[1] == wanted['target']
    return False

if not any(same_mount(v) for v in vols):
    vols.append(wanted)

with open(compose_file, 'w') as f:
    yaml.safe_dump(doc, f, sort_keys=False)

print('[OK] wrote', compose_file)
PY

echo "[pi] restarting faasd (gateway container)"
sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[OK] prototype gateway enabled"
