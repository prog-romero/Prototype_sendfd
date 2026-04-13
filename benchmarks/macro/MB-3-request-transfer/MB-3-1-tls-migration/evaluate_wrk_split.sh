#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# Split-host evaluation for MB-3.1
# - Servers run on the Raspberry Pi
# - Client load is generated locally using wrk (HTTPS)

PI_USER="${PI_USER:-romero}"
PI_HOST="${PI_HOST:-192.168.2.2}"
PI_BASEDIR="${PI_BASEDIR:-~/Prototype_sendfd}"

BENCH_REL="benchmarks/macro/MB-3-request-transfer/MB-3-1-tls-migration"
PI_BENCH="${PI_BASEDIR}/${BENCH_REL}"

WRK_BIN="${WRK_BIN:-wrk}"
WRK_THREADS="${WRK_THREADS:-1}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-1}"
WRK_DURATION="${WRK_DURATION:-2s}"
WRK_TIMEOUT="${WRK_TIMEOUT:-2s}"
WRK_EXTRA_ARGS="${WRK_EXTRA_ARGS:-}"

SSH_CMD_OPTS="-T -n -o BatchMode=yes -o ConnectTimeout=5"
SSH_RSYNC_OPTS="-T -o BatchMode=yes -o ConnectTimeout=5"

# Runtime link paths on the Pi (override if your Pi layout differs)
PI_LD_LIBRARY_PATH="${PI_LD_LIBRARY_PATH:-${PI_BASEDIR}/wolfssl/src/.libs:${PI_BASEDIR}/libtlspeek/build}"

PATH_A_URL="https://${PI_HOST}:8443/hello"
PATH_B_URL="https://${PI_HOST}:9001/hello"

remote() {
  ssh ${SSH_CMD_OPTS} "${PI_USER}@${PI_HOST}" "$@"
}

remote_kill_pidfile() {
  local pidfile="$1"
  remote "if [ -f '${pidfile}' ]; then kill \"\$(cat '${pidfile}')\" 2>/dev/null || true; rm -f '${pidfile}'; fi" >/dev/null 2>&1 || true
}

cleanup() {
  set +e
  remote_kill_pidfile /tmp/mb3_gw.pid
  remote_kill_pidfile /tmp/mb3_wm.pid
  remote_kill_pidfile /tmp/mb3_wc.pid
  remote "pkill -9 gateway worker_migration_complete worker_classic 2>/dev/null || true; pkill -9 -f 'bash -c cd .*MB-3-1-tls-migration' 2>/dev/null || true; rm -f /tmp/worker_migration.sock /tmp/mb3_*.pid" >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

wait_port() {
  local host="$1"
  local port="$2"
  local label="$3"

  for _ in $(seq 1 30); do
    if nc -z -w1 "$host" "$port" >/dev/null 2>&1; then
      echo "✓ ${label} reachable on ${host}:${port}"
      return 0
    fi
    sleep 1
  done

  echo "ERROR: ${label} not reachable on ${host}:${port}"
  return 1
}

echo "═══════════════════════════════════════════════════════════════"
echo "  MB-3.1 (Split Host): TLS Migration vs Fresh Handshake"
echo "  Servers: ${PI_USER}@${PI_HOST}   Client: $(hostname) (wrk)"
echo "═══════════════════════════════════════════════════════════════"
echo

echo "[INIT] Checking local prerequisites..."
command -v "${WRK_BIN}" >/dev/null 2>&1 || { echo "ERROR: wrk not found (set WRK_BIN or install wrk)"; exit 1; }
command -v ssh >/dev/null 2>&1 || { echo "ERROR: ssh not found"; exit 1; }
command -v rsync >/dev/null 2>&1 || { echo "ERROR: rsync not found"; exit 1; }
command -v nc >/dev/null 2>&1 || { echo "ERROR: nc (netcat) not found"; exit 1; }

echo "[INIT] Checking Pi connectivity..."
remote "echo '✓ Pi reachable'" >/dev/null

echo "[CLEANUP] Stopping any stale MB-3.1 servers on Pi..."
remote "pkill -9 gateway worker_migration_complete worker_classic 2>/dev/null || true; pkill -9 -f 'bash -c cd .*MB-3-1-tls-migration' 2>/dev/null || true; rm -f /tmp/worker_migration.sock /tmp/mb3_*.pid" >/dev/null 2>&1 || true

echo "[SYNC] Syncing MB-3.1 sources to Pi..."
rsync -az \
  -e "ssh ${SSH_RSYNC_OPTS}" \
  --include='*/' --include='*.c' --include='*.h' --include='Makefile' --include='*.py' --include='*.sh' \
  --exclude='*' \
  ./ "${PI_USER}@${PI_HOST}:${PI_BENCH}/" \
  >/dev/null
echo "✓ Source synced"

echo "[BUILD-PI] Building server binaries on Pi..."
remote "cd ${PI_BENCH} && make clean >/dev/null 2>&1 || true && make gateway worker_migration_complete worker_classic" \
  | grep -E "\[OK\]|error:" || true

echo "[CLEANUP] Clearing Pi results..."
remote "cd ${PI_BENCH} && rm -f results/mb3_1_*.csv && mkdir -p results" >/dev/null

mkdir -p results

echo
echo "═══════════════════════════════════════════════════════════════"
echo "  PHASE 1: PATH A (TLS State Migration)"
echo "═══════════════════════════════════════════════════════════════"

echo "[PI] Starting worker_migration_complete + gateway..."
remote "cd ${PI_BENCH} && \
  export LD_LIBRARY_PATH=${PI_LD_LIBRARY_PATH}:\$LD_LIBRARY_PATH && \
  rm -f /tmp/worker_migration.sock && \
  setsid -f ./worker_migration_complete </dev/null >/tmp/mb3_wm.log 2>&1 && \
  pgrep -n worker_migration_complete >/tmp/mb3_wm.pid" >/dev/null

remote "cd ${PI_BENCH} && \
  export LD_LIBRARY_PATH=${PI_LD_LIBRARY_PATH}:\$LD_LIBRARY_PATH && \
  setsid -f ./gateway </dev/null >/tmp/mb3_gw.log 2>&1 && \
  pgrep -n gateway >/tmp/mb3_gw.pid" >/dev/null

wait_port "${PI_HOST}" 8443 "Gateway (PATH A)"

echo "[WRK] ${PATH_A_URL}"
"${WRK_BIN}" -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" --latency \
  ${WRK_EXTRA_ARGS} \
  "${PATH_A_URL}" | tee "results/wrk_path_a.txt"

echo "[PI] Stopping PATH A servers..."
remote_kill_pidfile /tmp/mb3_gw.pid
remote_kill_pidfile /tmp/mb3_wm.pid
remote "rm -f /tmp/worker_migration.sock" >/dev/null 2>&1 || true

echo
echo "═══════════════════════════════════════════════════════════════"
echo "  PHASE 2: PATH B (Fresh TLS 1.3 Handshake)"
echo "═══════════════════════════════════════════════════════════════"

echo "[PI] Starting worker_classic..."
remote "cd ${PI_BENCH} && \
  export LD_LIBRARY_PATH=${PI_LD_LIBRARY_PATH}:\$LD_LIBRARY_PATH && \
  setsid -f ./worker_classic </dev/null >/tmp/mb3_wc.log 2>&1 && \
  pgrep -n worker_classic >/tmp/mb3_wc.pid" >/dev/null

wait_port "${PI_HOST}" 9001 "Worker classic (PATH B)"

echo "[WRK] ${PATH_B_URL}"
"${WRK_BIN}" -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" --latency \
  ${WRK_EXTRA_ARGS} \
  "${PATH_B_URL}" | tee "results/wrk_path_b.txt"

echo "[PI] Stopping PATH B server..."
remote_kill_pidfile /tmp/mb3_wc.pid

echo
echo "[FETCH] Pulling results back from Pi..."
rsync -az -e "ssh ${SSH_RSYNC_OPTS}" "${PI_USER}@${PI_HOST}:${PI_BENCH}/results/" ./results/ >/dev/null

echo "✓ Results fetched to ./results"

echo "[MERGE] Creating combined CSV (results/mb3_1_results.csv)..."
python3 - <<'PY'
import csv

def read_col(path: str):
    values = []
    try:
        with open(path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    values.append(float(line))
                except ValueError:
                    continue
    except FileNotFoundError:
        return []
    return values

gw = read_col('results/mb3_1_gateway_timings.csv')
wk = read_col('results/mb3_1_worker_timings.csv')
hs = read_col('results/mb3_1_handshake_timings.csv')

rows_a = min(len(gw), len(wk))
out_path = 'results/mb3_1_results.csv'

with open(out_path, 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['path', 'gateway_us', 'worker_us', 'handshake_us', 'total_us'])

    for i in range(rows_a):
        total = gw[i] + wk[i]
        w.writerow(['A', f'{gw[i]:.0f}', f'{wk[i]:.0f}', '0', f'{total:.0f}'])

    for h in hs:
        w.writerow(['B', '0', '0', f'{h:.0f}', f'{h:.0f}'])

print(f'[merge] wrote {out_path} (A={rows_a} rows, B={len(hs)} rows)')
PY

echo "[PLOT] Generating plots (results/mb3_1_comparison.png/.pdf)..."
python3 plot_results.py

echo
echo "═══════════════════════════════════════════════════════════════"
echo "  ✓ SPLIT-HOST BENCHMARK COMPLETE"
echo "═══════════════════════════════════════════════════════════════"
echo "Artifacts in ./results/:"
echo "  - mb3_1_gateway_timings.csv"
echo "  - mb3_1_worker_timings.csv"
echo "  - mb3_1_handshake_timings.csv"
echo "  - mb3_1_results.csv"
echo "  - mb3_1_comparison.png / .pdf"
echo "  - wrk_path_a.txt / wrk_path_b.txt"
