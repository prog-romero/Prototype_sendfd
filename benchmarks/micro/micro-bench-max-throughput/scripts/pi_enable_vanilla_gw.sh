#!/usr/bin/env bash
# pi_enable_vanilla_gw.sh
#
# Run ON THE PI to switch faasd to the bench3 vanilla direct-HTTPS gateway mode.
#
# Required env:
#   GATEWAY_IMAGE         — e.g. docker.io/local/bench3-keepalive-gateway:arm64
# Optional env:
#   PI_SUDO_PASSWORD      — sudo password for non-interactive runs
#   WAIT_HOST             — host to probe after restart (default: 127.0.0.1)
#   WAIT_PORT             — port to probe after restart (default: 8444)
#   WAIT_PATH             — HTTPS path to probe after restart (default: /healthz)
#   WAIT_RETRIES          — probe attempts after restart (default: 30)
#   WAIT_DELAY_SEC        — seconds between probe attempts (default: 1)

set -euo pipefail

: "${GATEWAY_IMAGE:?GATEWAY_IMAGE must be set}"
: "${WAIT_HOST:=127.0.0.1}"
: "${WAIT_PORT:=8444}"
: "${WAIT_PATH:=/healthz}"
: "${WAIT_RETRIES:=30}"
: "${WAIT_DELAY_SEC:=1}"

CPU_SET="${CPU_SET:-}"
NUM_CORES="${NUM_CORES:-}"

if [[ -n "${NUM_CORES}" ]]; then
    if [[ "${NUM_CORES}" -eq 1 ]]; then
        CPU_SET="0"
    else
        CPU_SET="0-$((NUM_CORES - 1))"
    fi
    echo "[pi-enable] CPU pinning enabled: ${NUM_CORES} core(s) (set: ${CPU_SET})"
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
sudo python3 - "${COMPOSE}" "${GATEWAY_IMAGE}" "${CPU_SET}" <<'PYEOF'
import sys, yaml

compose_path = sys.argv[1]
gateway_image = sys.argv[2]
cpu_set = sys.argv[3] if len(sys.argv) > 3 else ""


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
env_dict["BENCH2GW_ENABLE"] = "0"
for key in (
    "BENCH2GW_LISTEN",
    "BENCH2GW_CERT",
    "BENCH2GW_KEY",
    "BENCH2GW_SOCKET_DIR",
    "BENCH2GW_RELAY_SOCKET",
    "BENCH2GW_PEEK_BYTES",
):
    env_dict.pop(key, None)

for key in (
    "BENCH2_VANILLA_ENABLE",
    "BENCH2_VANILLA_LISTEN",
    "BENCH2_VANILLA_UPSTREAM",
    "BENCH2_VANILLA_CERT",
    "BENCH2_VANILLA_KEY",
):
    env_dict.pop(key, None)

env_dict["BENCH2_TLS_ENABLE"] = "1"
env_dict["BENCH2_TLS_LISTEN"] = ":8444"
env_dict["BENCH2_TLS_CERT"] = "/certs/server.crt"
env_dict["BENCH2_TLS_KEY"] = "/certs/server.key"
gw["environment"] = env_dict

ports = [port for port in gw.get("ports", []) if port != "9444:9444"]
if "8444:8444" not in ports:
    ports.append("8444:8444")
gw["ports"] = ports

vols = [vol for vol in gw.get("volumes", []) if vol != "./bench2:/run/bench2"]
gw["volumes"] = vols

if cpu_set:
    gw["cpuset"] = cpu_set
    num_cores = len(cpu_set.split(","))
    gw["cpus"] = float(num_cores)

with open(compose_path, "w") as fh:
    yaml.safe_dump(doc, fh, default_flow_style=False, allow_unicode=True, sort_keys=False)

print(f"[py] patched {compose_path} (pinned to {cpu_set if cpu_set else 'all'})")
PYEOF

echo "[pi-enable] restarting faasd"
sudo systemctl restart faasd

echo "[pi-enable] waiting for https://${WAIT_HOST}:${WAIT_PORT}${WAIT_PATH}"
for attempt in $(seq 1 "${WAIT_RETRIES}"); do
    if python3 - "${WAIT_HOST}" "${WAIT_PORT}" "${WAIT_PATH}" <<'PYEOF'
import ssl
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
path = sys.argv[3]

try:
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    with socket.create_connection((host, port), timeout=1.0) as raw_sock:
        with context.wrap_socket(raw_sock, server_hostname=host) as tls_sock:
            tls_sock.settimeout(2.0)
            request = (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                "Connection: close\r\n\r\n"
            )
            tls_sock.sendall(request.encode("ascii"))
            response = tls_sock.recv(256)

    if not response.startswith(b"HTTP/1."):
        raise RuntimeError("did not receive an HTTP status line")

    status_line = response.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
    parts = status_line.split()
    if len(parts) < 2:
        raise RuntimeError(f"malformed HTTP status line: {status_line}")

    status_code = int(parts[1])
    if status_code >= 500:
        raise RuntimeError(f"received HTTP {status_code}")
except Exception:
    sys.exit(1)
PYEOF
    then
        echo "[pi-enable] ready — bench3 vanilla direct HTTPS gateway answered on :${WAIT_PORT}"
        exit 0
    fi

    if [[ "${attempt}" == "${WAIT_RETRIES}" ]]; then
        echo "[pi-enable] https://${WAIT_HOST}:${WAIT_PORT}${WAIT_PATH} did not become ready after ${WAIT_RETRIES} attempts" >&2
        exit 1
    fi

    sleep "${WAIT_DELAY_SEC}"
done