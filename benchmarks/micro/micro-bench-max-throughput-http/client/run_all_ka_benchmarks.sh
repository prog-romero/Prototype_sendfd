#!/usr/bin/env bash
set -euo pipefail

# Automation script for micro-bench3-keepalive-http evaluation
# This script assumes both vanilla and proto stacks are ready on the Pi.

PI_IP="192.168.2.2"
RESULTS_DIR="../results"
mkdir -p "$RESULTS_DIR"

# 1. Vanilla - Single Owner (Baseline)
echo "--- Running Vanilla Single ---"
python3 run_keepalive_sweep.py \
  --host "$PI_IP" --port 8082 --mode single \
  --start-kb 32 --end-kb 2000 --step-kb 32 \
  --requests 50 --out "$RESULTS_DIR/vanilla_single.csv"

# 2. Vanilla - Switch Owner (Worst case for vanilla proxying)
echo "--- Running Vanilla Switch ---"
python3 run_keepalive_sweep.py \
  --host "$PI_IP" --port 8082 --mode switch \
  --start-kb 32 --end-kb 2000 --step-kb 32 \
  --requests 50 --out "$RESULTS_DIR/vanilla_switch.csv"

# 3. Proto - Single Owner (Baseline for prototype)
echo "--- Running Proto Single ---"
python3 run_keepalive_sweep.py \
  --host "$PI_IP" --port 8083 --mode single \
  --start-kb 32 --end-kb 2000 --step-kb 32 \
  --requests 50 --out "$RESULTS_DIR/proto_single.csv"

# 4. Proto - Switch Owner (Testing zero-copy migration with relay)
echo "--- Running Proto Switch ---"
python3 run_keepalive_sweep.py \
  --host "$PI_IP" --port 8083 --mode switch \
  --start-kb 32 --end-kb 2000 --step-kb 32 \
  --requests 50 --out "$RESULTS_DIR/proto_switch.csv"

echo "--- All benchmarks completed ---"
