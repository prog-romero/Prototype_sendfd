#!/usr/bin/env bash
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"
VANILLA_GATEWAY_IMAGE="${VANILLA_GATEWAY_IMAGE:-ghcr.io/openfaas/gateway:0.27.12}"

restore_from_backup=false
if sudo test -f "$BACKUP_FILE"; then
  if sudo env BACKUP_FILE="$BACKUP_FILE" python3 - <<'PY'
import os
import sys
import yaml

path = os.environ['BACKUP_FILE']
with open(path, 'r') as f:
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
    restore_from_backup=true
  fi
fi

if [ "$restore_from_backup" = true ]; then
  sudo cp "$BACKUP_FILE" "$COMPOSE_FILE"
else
  sudo env COMPOSE_FILE="$COMPOSE_FILE" VANILLA_GATEWAY_IMAGE="$VANILLA_GATEWAY_IMAGE" python3 - <<'PY'
import os
import yaml

compose_file = os.environ['COMPOSE_FILE']
vanilla_image = os.environ['VANILLA_GATEWAY_IMAGE']

with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f) or {}

services = doc.setdefault('services', {})
gw = services.get('gateway')
if not isinstance(gw, dict):
    raise SystemExit('ERROR: services.gateway not found or invalid')

gw['image'] = vanilla_image

env = gw.get('environment') or []
if isinstance(env, dict):
    for key in list(env.keys()):
        if str(key).startswith('TLSMIGRATE'):
            env.pop(key, None)
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')
    gw['environment'] = [item for item in env if not (isinstance(item, str) and item.startswith('TLSMIGRATE'))]

ports = gw.get('ports') or []
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')
gw['ports'] = [item for item in ports if not (isinstance(item, str) and item.split(':', 1)[0] in {'9443', '9444'})]

vols = gw.get('volumes') or []
if not isinstance(vols, list):
    raise SystemExit('ERROR: gateway.volumes must be a list')

def keep_mount(item):
    if isinstance(item, dict):
        return item.get('source') != './tlsmigrate' and item.get('target') != '/run/tlsmigrate'
    if isinstance(item, str):
        parts = item.split(':', 1)
        if len(parts) == 2:
            return parts[0] != './tlsmigrate' and parts[1] != '/run/tlsmigrate'
    return True

gw['volumes'] = [item for item in vols if keep_mount(item)]

with open(compose_file, 'w') as f:
    yaml.safe_dump(doc, f, sort_keys=False)
PY
fi

sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[ok] vanilla gateway restored"