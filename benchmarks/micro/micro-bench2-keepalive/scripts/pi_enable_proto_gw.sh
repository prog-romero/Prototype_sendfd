#!/usr/bin/env bash
# pi_enable_proto_gw.sh
#
# Run ON THE PI to switch faasd from the vanilla gateway to the bench2
# prototype gateway.
#
# Required env:
#   PROTO_GATEWAY_IMAGE   — e.g. docker.io/local/bench2-proto-gateway:arm64
# Optional env:
#   PI_SUDO_PASSWORD      — sudo password for non-interactive runs
#
# What it does:
#   1. Backs up docker-compose.yaml (once).
#   2. Uses Python + PyYAML to patch the gateway service:
#        - replaces the image
#        - injects BENCH2GW_* environment variables
#        - adds port 9444:9444
#        - adds volume mount ./bench2:/run/bench2
#   3. Restarts faasd.

set -euo pipefail

: "${PROTO_GATEWAY_IMAGE:?PROTO_GATEWAY_IMAGE must be set}"

if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "${PI_SUDO_PASSWORD}" | sudo -S -p '' -v
fi

COMPOSE="/var/lib/faasd/docker-compose.yaml"
BACKUP="${COMPOSE}.bench2-vanilla-backup"

if ! sudo test -f "${COMPOSE}"; then
    echo "[pi-enable] ${COMPOSE} not found" >&2
    exit 1
fi

if ! sudo test -f "${BACKUP}"; then
    echo "[pi-enable] backing up ${COMPOSE} → ${BACKUP}"
    sudo cp "${COMPOSE}" "${BACKUP}"
else
    echo "[pi-enable] backup already exists: ${BACKUP}"
fi

echo "[pi-enable] patching gateway image and env in ${COMPOSE}"
sudo python3 - "${COMPOSE}" "${PROTO_GATEWAY_IMAGE}" <<'PYEOF'
import sys, yaml

compose_path = sys.argv[1]
proto_image  = sys.argv[2]

with open(compose_path) as fh:
    doc = yaml.safe_load(fh)

gw = doc["services"]["gateway"]
gw["image"] = proto_image

# environment may be a list ["KEY=VAL", ...] or a dict {KEY: VAL}
raw_env = gw.get("environment", {})
if isinstance(raw_env, list):
    env_dict = {}
    for item in raw_env:
        if "=" in item:
            k, v = item.split("=", 1)
            env_dict[k] = v
        else:
            env_dict[item] = ""
else:
    env_dict = dict(raw_env) if raw_env else {}

env_dict["BENCH2GW_ENABLE"]       = "1"
env_dict["BENCH2GW_LISTEN"]       = ":9444"
env_dict["BENCH2GW_CERT"]         = "/certs/server.crt"
env_dict["BENCH2GW_KEY"]          = "/certs/server.key"
env_dict["BENCH2GW_SOCKET_DIR"]   = "/run/bench2"
env_dict["BENCH2GW_RELAY_SOCKET"] = "/run/bench2/relay.sock"
env_dict["BENCH2GW_PEEK_BYTES"]   = "8192"

# Write back as dict (cleaner yaml)
gw["environment"] = env_dict

ports = gw.setdefault("ports", [])
if "9444:9444" not in ports:
    ports.append("9444:9444")

vols = gw.setdefault("volumes", [])
if "./bench2:/run/bench2" not in vols:
    vols.append("./bench2:/run/bench2")

with open(compose_path, "w") as fh:
    yaml.dump(doc, fh, default_flow_style=False, allow_unicode=True)

print(f"[py] patched {compose_path}")
PYEOF

echo "[pi-enable] creating /var/lib/faasd/bench2 volume dir"
sudo mkdir -p /var/lib/faasd/bench2
sudo chmod 0777 /var/lib/faasd/bench2

echo "[pi-enable] restarting faasd"
sudo systemctl restart faasd
echo "[pi-enable] done — bench2 proto gateway should be listening on :9444"
