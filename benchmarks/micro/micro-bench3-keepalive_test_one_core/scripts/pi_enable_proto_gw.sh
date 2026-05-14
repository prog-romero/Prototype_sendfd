#!/usr/bin/env bash
# pi_enable_proto_gw.sh
#
# Run ON THE PI to switch faasd to the bench3 prototype keepalive gateway mode.
#
# Required env:
#   GATEWAY_IMAGE         — e.g. docker.io/local/bench3-keepalive-gateway:arm64
# Optional env:
#   PROTO_GATEWAY_IMAGE   — legacy alias for GATEWAY_IMAGE
#   PI_SUDO_PASSWORD      — sudo password for non-interactive runs

set -euo pipefail

GATEWAY_IMAGE="${GATEWAY_IMAGE:-${PROTO_GATEWAY_IMAGE:-}}"
if [[ -z "${GATEWAY_IMAGE}" ]]; then
    echo "[pi-enable] GATEWAY_IMAGE must be set" >&2
    exit 1
fi

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
sudo python3 - "${COMPOSE}" "${GATEWAY_IMAGE}" <<'PYEOF'
import sys, yaml

compose_path = sys.argv[1]
gateway_image = sys.argv[2]


def normalize_env(raw_env):
    if isinstance(raw_env, list):
        env_dict = {}
        for item in raw_env:
            if "=" in item:
                key, value = item.split("=", 1)
                env_dict[key] = value
            else:
                env_dict[item] = ""
        return env_dict
    return dict(raw_env) if raw_env else {}

with open(compose_path) as fh:
    doc = yaml.safe_load(fh)

gw = doc["services"]["gateway"]
gw["image"] = gateway_image

env_dict = normalize_env(gw.get("environment", {}))

env_dict["BENCH2GW_ENABLE"]       = "1"
env_dict["BENCH2GW_LISTEN"]       = ":9444"
env_dict["BENCH2GW_CERT"]         = "/certs/server.crt"
env_dict["BENCH2GW_KEY"]          = "/certs/server.key"
env_dict["BENCH2GW_SOCKET_DIR"]   = "/run/bench2"
env_dict["BENCH2GW_RELAY_SOCKET"] = "/run/bench2/relay.sock"
env_dict["BENCH2GW_PEEK_BYTES"]   = "65536"
for key in (
    "BENCH2_TLS_ENABLE",
    "BENCH2_TLS_LISTEN",
    "BENCH2_TLS_CERT",
    "BENCH2_TLS_KEY",
    "BENCH2_VANILLA_ENABLE",
    "BENCH2_VANILLA_LISTEN",
    "BENCH2_VANILLA_UPSTREAM",
    "BENCH2_VANILLA_CERT",
    "BENCH2_VANILLA_KEY",
):
    env_dict.pop(key, None)

gw["environment"] = env_dict

ports = [port for port in gw.get("ports", []) if port != "8444:8444"]
if "9444:9444" not in ports:
    ports.append("9444:9444")
gw["ports"] = ports

vols = list(gw.get("volumes", []))
if "./bench2:/run/bench2" not in vols:
    vols.append("./bench2:/run/bench2")
gw["volumes"] = vols

with open(compose_path, "w") as fh:
    yaml.safe_dump(doc, fh, default_flow_style=False, allow_unicode=True, sort_keys=False)

print(f"[py] patched {compose_path}")
PYEOF

echo "[pi-enable] creating /var/lib/faasd/bench2 volume dir"
sudo mkdir -p /var/lib/faasd/bench2
sudo chmod 0777 /var/lib/faasd/bench2

echo "[pi-enable] restarting faasd"
sudo systemctl restart faasd

echo "[pi-enable] waiting for gateway process to start (up to 15 s)"
GATEWAY_PID=""
for _i in $(seq 1 15); do
    GATEWAY_PID=$(pgrep -f '\./gateway' 2>/dev/null | head -1 || true)
    [[ -n "${GATEWAY_PID}" ]] && break
    sleep 1
done
if [[ -z "${GATEWAY_PID}" ]]; then
    echo "[pi-enable] warning: could not find gateway PID" >&2
fi

echo "[pi-enable] done — bench2 proto gateway should be listening on :9444"
