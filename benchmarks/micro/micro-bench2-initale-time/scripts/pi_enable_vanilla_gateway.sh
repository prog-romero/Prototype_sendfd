#!/usr/bin/env bash
# Enable the bench2 gateway image in vanilla direct-HTTPS mode.
# Run this ON THE PI.
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"

BENCH_GATEWAY_IMAGE="${BENCH_GATEWAY_IMAGE:-}"
if [ -z "$BENCH_GATEWAY_IMAGE" ]; then
  echo "ERROR: set BENCH_GATEWAY_IMAGE to your custom gateway image tag" >&2
  echo "Example: export BENCH_GATEWAY_IMAGE=ttl.sh/faasd-gateway-bench2-initale:24h" >&2
  echo "Example: export BENCH_GATEWAY_IMAGE=docker.io/local/faasd-gateway-bench2-initale:arm64" >&2
  exit 1
fi

if ! sudo test -f "$BACKUP_FILE"; then
  echo "[pi] backing up $COMPOSE_FILE -> $BACKUP_FILE"
  sudo cp "$COMPOSE_FILE" "$BACKUP_FILE"
fi

echo "[pi] patching gateway service in $COMPOSE_FILE for direct HTTPS benchmark mode"
sudo env COMPOSE_FILE="$COMPOSE_FILE" BENCH_GATEWAY_IMAGE="$BENCH_GATEWAY_IMAGE" python3 - <<'PY'
import os
import yaml

compose_file = os.environ['COMPOSE_FILE']
bench_image = os.environ['BENCH_GATEWAY_IMAGE']

with open(compose_file, 'r') as f:
    doc = yaml.safe_load(f)

services = doc.setdefault('services', {})
gw = services.get('gateway')
if not isinstance(gw, dict):
    raise SystemExit('ERROR: services.gateway not found or invalid')

gw['image'] = bench_image

env = gw.get('environment')
if env is None:
    env = []
    gw['environment'] = env

if isinstance(env, dict):
    for key in list(env.keys()):
        if str(key).startswith('TLSMIGRATE_'):
            env.pop(key, None)
    env['BENCH2_TLS_ENABLE'] = '1'
    env['BENCH2_TLS_LISTEN'] = ':8443'
    env['BENCH2_TLS_CERT'] = '/certs/server.crt'
    env['BENCH2_TLS_KEY'] = '/certs/server.key'
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')
    env[:] = [item for item in env if not (isinstance(item, str) and item.startswith('TLSMIGRATE_'))]

    def ensure(kv: str):
        key = kv.split('=', 1)[0]
        if not any(isinstance(x, str) and x.split('=', 1)[0] == key for x in env):
            env.append(kv)

    ensure('BENCH2_TLS_ENABLE=1')
    ensure('BENCH2_TLS_LISTEN=:8443')
    ensure('BENCH2_TLS_CERT=/certs/server.crt')
    ensure('BENCH2_TLS_KEY=/certs/server.key')

ports = gw.get('ports')
if ports is None:
    ports = []
    gw['ports'] = ports
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')

ports[:] = [p for p in ports if not (isinstance(p, str) and p.split(':', 1)[0] == '9443')]
if not any(isinstance(p, str) and p.split(':', 1)[0] == '8443' for p in ports):
    ports.append('8443:8443')

vols = gw.get('volumes')
if vols is None:
    vols = []
    gw['volumes'] = vols
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

print('[OK] wrote', compose_file)
PY

echo "[pi] restarting faasd (gateway container)"
sudo systemctl restart faasd
sudo systemctl status faasd --no-pager --full | head -60

echo "[OK] vanilla direct-HTTPS gateway enabled"