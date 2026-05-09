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
# Always patch from the clean backup so we never stack env-var corruption
run_sudo cat "$BACKUP_FILE" > "$TMP_COMPOSE_IN"
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

# For vanilla mode:
#   - Remove all proto-related env vars
#   - Remove stale BENCH3_HTTP_* vars so we can set them cleanly
#   - Run vanilla listener on :8085 (inside container) to avoid conflict
#     with gateway metrics that use :8082 in this image.
#   - Expose host :8082 -> container :8085 for backward compatibility.
if isinstance(env, dict):
    for key in list(env.keys()):
        if (
            str(key).startswith('HTTPMIGRATE_') or
            str(key).startswith('TLSMIGRATE_') or
            str(key).startswith('BENCH2_TLS_') or
            str(key).startswith('BENCH2GW_') or
            str(key).startswith('BENCH3_HTTP_')
        ):
            env.pop(key, None)
    env['BENCH3_HTTP_ENABLE'] = '1'
    env['BENCH3_HTTP_LISTEN'] = ':8085'
    env['BENCH3_HTTP_UPSTREAM'] = '127.0.0.1:8080'
else:
    if not isinstance(env, list):
        raise SystemExit('ERROR: gateway.environment must be list or dict')
    env[:] = [item for item in env if not (
        isinstance(item, str) and
        (
            item.startswith('HTTPMIGRATE_') or
            item.startswith('TLSMIGRATE_') or
            item.startswith('BENCH2_TLS_') or
            item.startswith('BENCH2GW_') or
            item.startswith('BENCH3_HTTP_')
        )
    )]
    env.append('BENCH3_HTTP_ENABLE=1')
    env.append('BENCH3_HTTP_LISTEN=:8085')
    env.append('BENCH3_HTTP_UPSTREAM=127.0.0.1:8080')

ports = gw.get('ports')
if ports is None:
    ports = []
    gw['ports'] = ports
if not isinstance(ports, list):
    raise SystemExit('ERROR: gateway.ports must be a list')

# Vanilla mode: remove proto-related listener ports; ensure host 8082 maps
# to container 8085 (vanilla listener)
ports[:] = [p for p in ports if not (
    isinstance(p, str) and p.split(':', 1)[0] in ('8082', '8083', '8444', '9443')
)]
ports.append('8082:8085')

vols = gw.get('volumes')
if vols is None:
    vols = []
    gw['volumes'] = vols
if not isinstance(vols, list):
    raise SystemExit('ERROR: gateway.volumes must be a list')

def keep_mount(item):
    if isinstance(item, dict):
        source = item.get('source')
        target = item.get('target')
        return (
            source != './httpmigrate' and target != '/run/httpmigrate' and
            source != './tlsmigrate' and target != '/run/tlsmigrate'
        )
    if isinstance(item, str):
        parts = item.split(':', 1)
        if len(parts) == 2:
            return (
                parts[0] != './httpmigrate' and parts[1] != '/run/httpmigrate' and
                parts[0] != './tlsmigrate' and parts[1] != '/run/tlsmigrate'
            )
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
