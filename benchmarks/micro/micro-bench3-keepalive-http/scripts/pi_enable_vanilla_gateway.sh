#!/usr/bin/env bash
# Enable the bench2 gateway image in vanilla HTTP benchmark mode.
# Run this ON THE PI.
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-/var/lib/faasd/docker-compose.yaml}"
BACKUP_FILE="${BACKUP_FILE:-/var/lib/faasd/docker-compose.vanilla.yaml}"
PI_SUDO_PASSWORD="${PI_SUDO_PASSWORD:-}"

SUDO=(sudo)
if [ -n "$PI_SUDO_PASSWORD" ]; then
  SUDO=(sudo -S -p "")
fi

run_sudo() {
  if [ -n "$PI_SUDO_PASSWORD" ]; then
    printf '%s\n' "$PI_SUDO_PASSWORD" | "${SUDO[@]}" "$@"
  else
    "${SUDO[@]}" "$@"
  fi
}

BENCH_GATEWAY_IMAGE="${BENCH_GATEWAY_IMAGE:-}"
if [ -z "$BENCH_GATEWAY_IMAGE" ]; then
  echo "ERROR: set BENCH_GATEWAY_IMAGE to your custom gateway image tag" >&2
  echo "Example: export BENCH_GATEWAY_IMAGE=docker.io/local/faasd-gateway-bench2-initial-http:arm64" >&2
  exit 1
fi

if ! run_sudo test -f "$BACKUP_FILE"; then
  echo "[pi] backing up $COMPOSE_FILE -> $BACKUP_FILE"
  run_sudo cp "$COMPOSE_FILE" "$BACKUP_FILE"
fi

echo "[pi] patching gateway service in $COMPOSE_FILE for vanilla HTTP benchmark mode"
TMP_COMPOSE_IN="$(mktemp)"
TMP_COMPOSE_OUT="$(mktemp)"
run_sudo cat "$COMPOSE_FILE" > "$TMP_COMPOSE_IN"
python3 - "$TMP_COMPOSE_IN" "$BENCH_GATEWAY_IMAGE" "$TMP_COMPOSE_OUT" <<'PY'
import sys
import yaml

compose_file = sys.argv[1]
bench_image = sys.argv[2]
tmp_compose = sys.argv[3]

with open(compose_file, 'r', encoding='utf-8') as f:
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
        if str(key).startswith('HTTPMIGRATE_'):
            env.pop(key, None)
    env['BENCH3_HTTP_ENABLE'] = '1'
    env['BENCH3_HTTP_LISTEN'] = ':8085'
    env['BENCH3_HTTP_UPSTREAM'] = '127.0.0.1:8080'
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')
    env[:] = [item for item in env if not (isinstance(item, str) and item.startswith('HTTPMIGRATE_'))]

    def ensure(kv: str):
        key = kv.split('=', 1)[0]
        # Remove old key if it exists to ensure we overwrite with new value
        env[:] = [x for x in env if not (isinstance(x, str) and x.split('=', 1)[0] == key)]
        env.append(kv)

    ensure('BENCH3_HTTP_ENABLE=1')
    ensure('BENCH3_HTTP_LISTEN=:8085')
    ensure('BENCH3_HTTP_UPSTREAM=127.0.0.1:8080')

ports = gw.get('ports')
if ports is None:
    ports = []
    gw['ports'] = ports
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')

ports[:] = [p for p in ports if not (isinstance(p, str) and p.split(':', 1)[0] == '8083')]
# Remove any existing 8082 mapping so we can add the 8082:8085 one cleanly
ports[:] = [p for p in ports if not (isinstance(p, str) and p.split(':', 1)[0] == '8082')]
ports.append('8082:8085')

vols = gw.get('volumes')
if vols is None:
    vols = []
    gw['volumes'] = vols
if not isinstance(vols, list):
    raise SystemExit('ERROR: gateway.volumes must be a list')

def keep_mount(item):
    if isinstance(item, dict):
        return item.get('source') != './httpmigrate' and item.get('target') != '/run/httpmigrate'
    if isinstance(item, str):
        parts = item.split(':', 1)
        if len(parts) == 2:
            return parts[0] != './httpmigrate' and parts[1] != '/run/httpmigrate'
    return True

gw['volumes'] = [item for item in vols if keep_mount(item)]

with open(tmp_compose, 'w', encoding='utf-8') as f:
    yaml.safe_dump(doc, f, sort_keys=False)

print('[OK] wrote', tmp_compose)
PY
run_sudo cp "$TMP_COMPOSE_OUT" "$COMPOSE_FILE"
rm -f "$TMP_COMPOSE_IN" "$TMP_COMPOSE_OUT"

echo "[pi] restarting faasd"
run_sudo systemctl restart faasd
run_sudo systemctl status faasd --no-pager --full | head -60

echo "[OK] vanilla HTTP benchmark gateway enabled"
