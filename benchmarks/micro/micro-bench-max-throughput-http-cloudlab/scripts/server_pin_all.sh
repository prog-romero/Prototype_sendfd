#!/usr/bin/env bash
# server_pin_all.sh
#
# Run on the server (via SSH) to pin gateway/function containers to NUM_CORES.

set -euo pipefail

: "${NUM_CORES:?NUM_CORES must be set (1, 2, 3, or 4)}"
: "${FAASD_FN_NAMESPACE:=openfaas-fn}"
: "${GATEWAY_SERVICE:=gateway}"

if [[ "${NUM_CORES}" -eq 1 ]]; then
    CPU_SET="0"
else
    CPU_SET="0-$((NUM_CORES - 1))"
fi

AFFINITY_MASK=$(python3 -c "print(hex((1 << ${NUM_CORES}) - 1))")

echo "[pin] Pinning all server processes to CPUs: ${CPU_SET} (mask=${AFFINITY_MASK})"

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
        sudo taskset -p "${AFFINITY_MASK}" "${tid}" >/dev/null 2>&1 && count=$((count + 1)) || true
    done
    for child_pid in $(pgrep -P "${pid}" 2>/dev/null || true); do
        for tid in $(ls /proc/${child_pid}/task/ 2>/dev/null || true); do
            sudo taskset -p "${AFFINITY_MASK}" "${tid}" >/dev/null 2>&1 && count=$((count + 1)) || true
        done
    done
    echo "  [ok] ${label} (PID=${pid}): pinned ${count} thread(s) to CPUs ${CPU_SET}"
}

pin_cgroup_of_pid() {
    local pid="$1"
    local label="$2"
    if [[ -z "${pid}" ]] || ! [[ "${pid}" =~ ^[0-9]+$ ]]; then
        return 0
    fi

    local cgv2
    cgv2=$(cat /proc/${pid}/cgroup 2>/dev/null | awk -F: '/^0::/{print $3}' | head -1)
    if [[ -n "${cgv2}" ]] && [[ -f "/sys/fs/cgroup${cgv2}/cpuset.cpus" ]]; then
        echo "${CPU_SET}" | sudo tee "/sys/fs/cgroup${cgv2}/cpuset.cpus" >/dev/null
        echo "0" | sudo tee "/sys/fs/cgroup${cgv2}/cpuset.mems" >/dev/null
        echo "  [cgroup-v2] ${label}: cpuset.cpus = ${CPU_SET}"
        return 0
    fi

    local cgv1
    cgv1=$(cat /proc/${pid}/cgroup 2>/dev/null | awk -F: '/:cpuset:/{print $3}' | head -1)
    if [[ -n "${cgv1}" ]] && [[ -f "/sys/fs/cgroup/cpuset${cgv1}/cpuset.cpus" ]]; then
        echo "${CPU_SET}" | sudo tee "/sys/fs/cgroup/cpuset${cgv1}/cpuset.cpus" >/dev/null
        echo "0" | sudo tee "/sys/fs/cgroup/cpuset${cgv1}/cpuset.mems" >/dev/null
        echo "  [cgroup-v1] ${label}: cpuset.cpus = ${CPU_SET}"
        return 0
    fi

    echo "  [warn] ${label}: could not find cpuset cgroup for PID ${pid}"
}

echo ""
echo "=== [1/2] Pinning gateway container ==="

GW_PID=""
if command -v ctr >/dev/null 2>&1; then
    GW_PID=$(sudo ctr -n openfaas task ls 2>/dev/null | awk "/${GATEWAY_SERVICE}/{print \$2}" | head -1 || true)
fi

if [[ -z "${GW_PID}" ]]; then
    GW_PID=$(pgrep -f "bench.*gateway\|bench3.*gw\|benchhttps" 2>/dev/null | head -1 || true)
fi

if [[ -z "${GW_PID}" ]]; then
    echo "  [warn] gateway container PID not found; gateway may not be running yet"
else
    pin_pid_tree "${GW_PID}" "gateway"
    pin_cgroup_of_pid "${GW_PID}" "gateway"
fi

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
            if [[ "${status}" != "RUNNING" ]]; then
                echo "  [skip] container ${cid} is ${status}"
                continue
            fi
            pin_pid_tree "${pid}" "fn:${cid}"
            pin_cgroup_of_pid "${pid}" "fn:${cid}"
        done
    fi
fi

echo ""
echo "[pin] Done. Gateway + function containers pinned to CPUs: ${CPU_SET}"
