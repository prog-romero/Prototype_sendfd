#!/usr/bin/env bash
# run_max_throughput.sh
#
# Master script for evaluating maximum throughput of Vanilla vs Prototype modes
# with configurable CPU pinning.

set -euo pipefail

# --- Configuration ---
: "${PI_IP:=192.168.2.2}"
: "${PI_SSH:=romero@${PI_IP}}"
: "${NUM_CORES:=1}"  # Number of cores to pin to (1 or 2)
: "${CONNECTIONS:=2 4 8 16 32 64 128}" # Connection sweep
: "${DURATION:=30s}"
: "${THREADS:=2}"     # wrk threads (local)

# Prompt for Pi sudo password if not provided
if [[ -z "${PI_SUDO_PASSWORD:-}" ]]; then
    echo -n "Enter sudo password for ${PI_SSH}: "
    read -s PI_SUDO_PASSWORD
    echo
fi

export PI_SUDO_PASSWORD

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}/.."
CLIENT_DIR="${BASE_DIR}/client"
RESULTS_DIR="${BASE_DIR}/results/throughput"
mkdir -p "${RESULTS_DIR}"

export NUM_CORES
export PI_SSH

# --- Helper: Run wrk sweep ---
run_wrk_sweep() {
    local label="$1"
    local url="$2"
    local lua_script="${3:-}"
    local outfile="${RESULTS_DIR}/${label}.csv"

    echo "payload_bytes,connections,rps,transfer_mb_s" > "${outfile}"
    
    for c in ${CONNECTIONS}; do
        echo "--- Testing ${label} with ${c} connections ---"
        local cmd=(wrk -t"${THREADS}" -c"${c}" -d"${DURATION}" --latency)
        if [[ -n "${lua_script}" ]]; then
            cmd+=(-s "${lua_script}")
        fi
        cmd+=("${url}")

        # Run wrk and extract RPS and Transfer rate
        local output
        output=$("${cmd[@]}")
        echo "${output}"

        local rps=$(echo "${output}" | grep "Requests/sec:" | awk '{print $2}')
        local transfer=$(echo "${output}" | grep "Transfer/sec:" | awk '{print $2}')
        
        # Convert transfer to MB/s if needed (wrk outputs with units)
        echo "32768,${c},${rps},${transfer}" >> "${outfile}"
    done
}

# --- 1. Prepare Stacks ---
echo "=== [1/4] Building and Deploying Workers ==="
# We assume workers are already built or we run the build script once
bash "${BASE_DIR}/scripts/build_push_deploy_proto_worker.sh"
# Vanilla worker is usually the same binary or a slightly different one, 
# but for throughput with 32KB, the proto worker binary can also run in vanilla mode 
# if the gateway doesn't send the FD. 
# However, to be strict, we deploy the vanilla function.
bash "${BASE_DIR}/scripts/build_push_deploy_vanilla_function.sh"

# --- 2. Vanilla Evaluation ---
echo "=== [2/4] Vanilla Evaluation (Pinned to ${NUM_CORES} cores) ==="
bash "${BASE_DIR}/scripts/prepare_vanilla_stack.sh"

echo "--- Scenario: Same Function (Baseline) ---"
run_wrk_sweep "vanilla_same_cores${NUM_CORES}" "https://${PI_IP}:8444/function/bench2-fn-a"

echo "--- Scenario: Alternating (Migration overhead) ---"
run_wrk_sweep "vanilla_alt_cores${NUM_CORES}" "https://${PI_IP}:8444/" "${CLIENT_DIR}/alternate.lua"

# --- 3. Prototype Evaluation ---
echo "=== [3/4] Prototype Evaluation (Pinned to ${NUM_CORES} cores) ==="
bash "${BASE_DIR}/scripts/prepare_proto_stack.sh"

echo "--- Scenario: Same Function (Baseline) ---"
run_wrk_sweep "proto_same_cores${NUM_CORES}" "https://${PI_IP}:9444/function/bench2-fn-a"

echo "--- Scenario: Alternating (Relay overhead) ---"
run_wrk_sweep "proto_alt_cores${NUM_CORES}" "https://${PI_IP}:9444/" "${CLIENT_DIR}/alternate.lua"

echo "=== [4/4] Evaluation Complete ==="
echo "Results are in ${RESULTS_DIR}"
