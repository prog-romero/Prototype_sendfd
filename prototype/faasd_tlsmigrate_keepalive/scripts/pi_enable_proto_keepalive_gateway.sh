#!/usr/bin/env bash
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"
PROTO_GATEWAY_IMAGE="${PROTO_GATEWAY_IMAGE:-}"

if [ -z "$PROTO_GATEWAY_IMAGE" ]; then
  echo "ERROR: set PROTO_GATEWAY_IMAGE to your custom keepalive gateway image tag" >&2
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

def has_proto_env(seq):
    if isinstance(seq, dict):
        return any(str(k).startswith('TLSMIGRATE') for k in seq)
    if isinstance(seq, list):
        return any(isinstance(x, str) and x.startswith('TLSMIGRATE') for x in seq)
    return False

def has_proto_port(seq):
    if not isinstance(seq, list):
        return False
    for item in seq:
        if isinstance(item, str) and item.split(':', 1)[0] in {'9443', '9444'}:
            return True
    return False

def has_mount(seq):
    if not isinstance(seq, list):
        return False
    for item in seq:
        if isinstance(item, dict) and item.get('source') == './tlsmigrate':
            return True
        if isinstance(item, str) and item.split(':', 1)[0] == './tlsmigrate':
            return True
    return False

is_proto = ('faasd-gateway-tlsmigrate' in image) or has_proto_env(env) or has_proto_port(ports) or has_mount(vols)
sys.exit(1 if is_proto else 0)
PY
  then
    sudo cp "$COMPOSE_FILE" "$BACKUP_FILE"
  fi
fi

sudo env COMPOSE_FILE="$COMPOSE_FILE" PROTO_GATEWAY_IMAGE="$PROTO_GATEWAY_IMAGE" python3 - <<'PY'
import os
import yaml

compose_file = os.environ['COMPOSE_FILE']
proto_image = os.environ['PROTO_GATEWAY_IMAGE']

with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f)

services = doc.setdefault('services', {})
gw = services.get('gateway')
if not isinstance(gw, dict):
    raise SystemExit('ERROR: services.gateway not found or invalid')

gw['image'] = proto_image

env = gw.get('environment')
if env is None:
    env = []
    gw['environment'] = env

keepalive_env = {
    'TLSMIGRATE_KEEPALIVE_ENABLE': '1',
    'TLSMIGRATE_KEEPALIVE_LISTEN': ':9444',
    'TLSMIGRATE_KEEPALIVE_SOCKET_DIR': '/run/tlsmigrate',
    'TLSMIGRATE_KEEPALIVE_RELAY_SOCKET': '/run/tlsmigrate/keepalive-relay.sock',
    'TLSMIGRATE_KEEPALIVE_CERT': '/certs/server.crt',
    'TLSMIGRATE_KEEPALIVE_KEY': '/certs/server.key',
}

def remove_old_keys_dict(dct):
    for key in list(dct.keys()):
        if str(key).startswith('TLSMIGRATE_'):
            dct.pop(key, None)

def remove_old_keys_list(seq):
    return [item for item in seq if not (isinstance(item, str) and item.startswith('TLSMIGRATE_'))]

if isinstance(env, dict):
    remove_old_keys_dict(env)
    env.update(keepalive_env)
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')
    env = remove_old_keys_list(env)
    for key, value in keepalive_env.items():
        env.append(f'{key}={value}')
    gw['environment'] = env

ports = gw.get('ports') or []
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')
ports = [item for item in ports if not (isinstance(item, str) and item.split(':', 1)[0] in {'9443', '9444'})]
ports.append('9444:9444')
gw['ports'] = ports

vols = gw.get('volumes') or []
if not isinstance(vols, list):
    raise SystemExit('ERROR: gateway.volumes must be a list')

def same_mount(v):
    if isinstance(v, dict):
        return v.get('source') == './tlsmigrate' and v.get('target') == '/run/tlsmigrate'
    if isinstance(v, str):
        parts = v.split(':', 1)
        return len(parts) == 2 and parts[0] == './tlsmigrate' and parts[1] == '/run/tlsmigrate'
    return False

if not any(same_mount(v) for v in vols):
    vols.append({'type': 'bind', 'source': './tlsmigrate', 'target': '/run/tlsmigrate'})
gw['volumes'] = vols

with open(compose_file, 'w') as f:
    yaml.safe_dump(doc, f, sort_keys=False)
PY

sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[ok] keepalive gateway enabled"