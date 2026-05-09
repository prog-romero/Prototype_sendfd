#!/usr/bin/env bash
# pi_pin_all.sh
#
# Run ON THE PI (via SSH) to pin ALL server processes to exactly N CPU cores.
# This covers: faasd, the gateway container, and all function containers
# (bench2-fn-a, bench2-fn-b).
#
# Usage:
#   NUM_CORES=2 bash pi_pin_all.sh             # on the Pi as root
#   # or via SSH from client (sudo env is required to pass vars through sudo):
#   ssh romero@192.168.2.2 "echo PASS | sudo -S env NUM_CORES=2 bash /tmp/pi_pin_all.sh"
#
# Required env:
#   NUM_CORES — number of cores to use (1, 2, 3, or 4)
#
# Optional env:
#   PI_SUDO_PASSWORD — if set, used to acquire sudo non-interactively
#   FAASD_FN_NAMESPACE — containerd namespace for faasd functions
#                        (default: openfaas-fn)
#   GATEWAY_SERVICE    — containerd/docker-compose service name pattern
#                        (default: gateway)

set -euo pipefail

: "${NUM_CORES:?NUM_CORES must be set (1, 2, 3, or 4)}"
: "${FAASD_FN_NAMESPACE:=openfaas-fn}"
: "${GATEWAY_SERVICE:=gateway}"

if [[ -n "${PI_SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "${PI_SUDO_PASSWORD}" | sudo -S -p '' -v 2>/dev/null
fi

# Build CPU set string: "0" for 1 core, "0-1" for 2, "0-2" for 3, "0-3" for 4
if [[ "${NUM_CORES}" -eq 1 ]]; then
    CPU_SET="0"
else
    CPU_SET="0-$((NUM_CORES - 1))"
fi

# Build the affinity mask: 1 core → 0x1, 2 cores → 0x3, 3 → 0x7, 4 → 0xf
AFFINITY_MASK=$(python3 -c "print(hex((1 << ${NUM_CORES}) - 1))")

echo "[pi-pin] Pinning all server processes to CPUs: ${CPU_SET} (mask=${AFFINITY_MASK})"

# ---------------------------------------------------------------------------
# Helper: pin all threads of a process tree to the CPU set
# ---------------------------------------------------------------------------
pin_pid_tree() {
    local pid="$1"
    local label="$2"
    if [[ -z "${pid}" ]] || ! [[ "${pid}" =~ ^[0-9]+$ ]]; then
        echo "  [skip] ${label}: invalid PID '${pid}'"
        return 0
    fi
    if ! sudo kill -0 "${pid}" 2>/dev/null; then
        echo "  [skip] ${label}: PID ${pid} not running"
        return 0
    fi
    local tids
    tids=$(ls /proc/${pid}/task/ 2>/dev/null || true)
    local count=0
    for tid in ${tids}; do
        sudo taskset -p "${AFFINITY_MASK}" "${tid}" >/dev/null 2>&1 && count=$((count+1)) || true
    done
    # Also pin child processes
    for child_pid in $(pgrep -P "${pid}" 2>/dev/null || true); do
        for tid in $(ls /proc/${child_pid}/task/ 2>/dev/null || true); do
            sudo taskset -p "${AFFINITY_MASK}" "${tid}" >/dev/null 2>&1 && count=$((count+1)) || true
        done
    done
    echo "  [ok] ${label} (PID=${pid}): pinned ${count} thread(s) to CPUs ${CPU_SET}"
}

# ---------------------------------------------------------------------------
# Helper: pin a cgroup to the CPU set (both cgroup v1 and v2)
# ---------------------------------------------------------------------------
pin_cgroup_of_pid() {
    local pid="$1"
    local label="$2"
    if [[ -z "${pid}" ]] || ! [[ "${pid}" =~ ^[0-9]+$ ]]; then
        return 0
    fi

    # Try cgroup v2 first (single hierarchy, path under /sys/fs/cgroup)
    local cgv2
    cgv2=$(cat /proc/${pid}/cgroup 2>/dev/null | awk -F: '/^0::/{print $3}' | head -1)
    if [[ -n "${cgv2}" ]] && [[ -f "/sys/fs/cgroup${cgv2}/cpuset.cpus" ]]; then
        echo "${CPU_SET}" | sudo tee "/sys/fs/cgroup${cgv2}/cpuset.cpus" >/dev/null
        echo "0"          | sudo tee "/sys/fs/cgroup${cgv2}/cpuset.mems" >/dev/null
        echo "  [cgroup-v2] ${label}: cpuset.cpus = ${CPU_SET}"
        return 0
    fi

    # Fall back to cgroup v1 cpuset hierarchy
    local cgv1
    cgv1=$(cat /proc/${pid}/cgroup 2>/dev/null | awk -F: '/:cpuset:/{print $3}' | head -1)
    if [[ -n "${cgv1}" ]] && [[ -f "/sys/fs/cgroup/cpuset${cgv1}/cpuset.cpus" ]]; then
        echo "${CPU_SET}" | sudo tee "/sys/fs/cgroup/cpuset${cgv1}/cpuset.cpus" >/dev/null
        echo "0"          | sudo tee "/sys/fs/cgroup/cpuset${cgv1}/cpuset.mems" >/dev/null
        echo "  [cgroup-v1] ${label}: cpuset.cpus = ${CPU_SET}"
        return 0
    fi

    echo "  [warn] ${label}: could not find cpuset cgroup for PID ${pid}"
}

# ---------------------------------------------------------------------------
# 1. Pin the gateway container process
#    The gateway is managed by faasd/containerd in namespace "openfaas".
#    Its main process shows up as the containerd task.
# ---------------------------------------------------------------------------
echo ""
echo "=== [1/2] Pinning gateway container ==="

GW_PID=""

# Try containerd task in "openfaas" namespace
if command -v ctr >/dev/null 2>&1; then
    GW_PID=$(sudo ctr -n openfaas task ls 2>/dev/null \
        | awk "/${GATEWAY_SERVICE}/{print \$2}" | head -1 || true)
fi

# Fallback: look for the gateway binary in /proc by its common name
if [[ -z "${GW_PID}" ]]; then
    GW_PID=$(pgrep -f "bench.*gateway\|bench3.*gw\|benchhttps" 2>/dev/null | head -1 || true)
fi

if [[ -z "${GW_PID}" ]]; then
    echo "  [warn] gateway container PID not found; gateway may not be running yet"
else
    pin_pid_tree "${GW_PID}" "gateway"
    pin_cgroup_of_pid "${GW_PID}" "gateway"
fi

# ---------------------------------------------------------------------------
# 2. Pin function containers (bench2-fn-a, bench2-fn-b)
#    They run in the containerd namespace "openfaas-fn" managed by faasd.
# ---------------------------------------------------------------------------
echo ""
echo "=== [2/2] Pinning function containers (${FAASD_FN_NAMESPACE}) ==="

if ! command -v ctr >/dev/null 2>&1; then
    echo "  [warn] ctr not found; cannot pin function containers via containerd"
else
    TASK_LIST=$(sudo ctr -n "${FAASD_FN_NAMESPACE}" task ls 2>/dev/null || true)
    if [[ -z "${TASK_LIST}" ]]; then
        echo "  [warn] no tasks found in namespace ${FAASD_FN_NAMESPACE}"
    else
        echo "${TASK_LIST}" | tail -n +2 | while read -r cid pid status rest; do
            if [[ -z "${pid}" ]] || ! [[ "${pid}" =~ ^[0-9]+$ ]]; then
                continue
            fi
            # Only pin RUNNING containers
            if [[ "${status}" != "RUNNING" ]]; then
                echo "  [skip] container ${cid} is ${status}"
                continue
            fi
            pin_pid_tree "${pid}" "fn:${cid}"
            pin_cgroup_of_pid "${pid}" "fn:${cid}"
        done
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "[pi-pin] Done. Gateway + function containers pinned to CPUs: ${CPU_SET}"
echo ""
echo "Verify with:"
echo "  sudo ctr -n openfaas-fn task ls"
echo "  for pid in \$(sudo ctr -n openfaas-fn task ls | tail -n +2 | awk '{print \$2}'); do"
echo "      taskset -p \$pid; done"
