#!/bin/bash
# collect_pidstat.sh
#
# Collects per-component CPU and RAM metrics using pidstat during throughput evaluation.
#
# KEY DESIGN:
#   - ONE pidstat process monitors ALL components simultaneously → perfect time alignment.
#   - Samples every 1 second; results are aggregated into windows of --interval seconds.
#   - Within each window the MEAN (average) of observed values is kept.
#   - awk writes directly to per-component CSV files: all files have the same row count
#     and line N in gateway.csv corresponds exactly to line N in faasd.csv, etc.
#
# Usage:
#   ./collect_pidstat.sh --mode <vanilla|prototype> \
#                        --duration <seconds>        \
#                        --interval <seconds>
#
#   --mode      vanilla   : gateway, faasd, fwatchdog, vanilla-fn-work
#               prototype : gateway, faasd, fwatchdog, timing-fn-ka-wo
#   --duration  total collection time in seconds (= number of wrk2 rate steps × step duration)
#   --interval  aggregation window in seconds    (= your wrk2 window / step duration)
#
# Output:
#   pidstat/<mode>/cpu/<component>.csv   → timestamp, usr_pct, system_pct, cpu_pct
#   pidstat/<mode>/ram/<component>.csv   → timestamp, minflt_s, majflt_s, vsz_kb, rss_kb, mem_pct

set -euo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────────

usage() {
    echo "Usage: $0 --mode <vanilla|prototype> --duration <seconds> --interval <seconds>"
    exit 1
}

MODE=""
DURATION=""
INTERVAL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)     MODE="$2";     shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --interval) INTERVAL="$2"; shift 2 ;;
        -h|--help)  usage ;;
        *) echo "Unknown argument: $1"; usage ;;
    esac
done

[[ -z "$MODE" || -z "$DURATION" || -z "$INTERVAL" ]] && usage

if [[ "$MODE" != "vanilla" && "$MODE" != "prototype" ]]; then
    echo "Error: --mode must be 'vanilla' or 'prototype'"
    exit 1
fi

# ── Components by mode ────────────────────────────────────────────────────────
# Process names are the first 15 characters of the binary (Linux comm limit).
#   vanilla   : vanilla-fn-worker  (17 chars) → comm = "vanilla-fn-work"
#   prototype : timing-fn-ka-worker(19 chars) → comm = "timing-fn-ka-wo"

if [[ "$MODE" == "vanilla" ]]; then
    COMP_LIST=("gateway" "faasd" "fwatchdog" "vanilla-fn-work")
else
    COMP_LIST=("gateway" "faasd" "fwatchdog" "timing-fn-ka-wo")
fi

COMP_STR="${COMP_LIST[*]}"   # space-separated string passed to awk

# ── Output directories ────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/$MODE"
CPU_DIR="$OUT_DIR/cpu"
RAM_DIR="$OUT_DIR/ram"

mkdir -p "$CPU_DIR" "$RAM_DIR"

# ── Write CSV headers (overwrites any previous run) ───────────────────────────

for COMP in "${COMP_LIST[@]}"; do
    printf "timestamp,usr_pct,system_pct,cpu_pct\n"               > "$CPU_DIR/${COMP}.csv"
    printf "timestamp,minflt_s,majflt_s,vsz_kb,rss_kb,mem_pct\n" > "$RAM_DIR/${COMP}.csv"
done

echo "======================================================================"
echo "[pidstat] mode=$MODE  duration=${DURATION}s  interval=${INTERVAL}s  (MEAN per window)"
echo "[pidstat] output: $OUT_DIR"
echo "======================================================================"

# ── awk — CPU metrics (pidstat -u) ───────────────────────────────────────────
#
# pidstat -u column order (sysstat 12.x, aarch64) — 10 fields:
#   $1=Time  $2=UID  $3=PID  $4=%usr  $5=%system  $6=%guest  $7=%wait  $8=%CPU  $9=CPU  $10=Command
#
# We read from the END so the script is robust to versions that add/remove
# columns in the middle:
#   $NF     = Command
#   $(NF-2) = %CPU   (total cpu%)
#   $(NF-5) = %system
#   $(NF-6) = %usr
#
# WINDOW LOGIC:
#   - A new second is detected when the timestamp ($1) changes.
#   - sample_num counts how many distinct seconds have been seen in the current window.
#   - When sample_num reaches INTERVAL, flush (write mean row per component) and reset.
#   - Every component always gets a row per window (value = 0.0 if the process was
#     not seen), which guarantees that all CSV files have the same row count.
#   - Mean = sum of 1-second samples / number of samples seen (win_cnt[c]).

CPU_AWK='
BEGIN {
    n = split(comp_list, arr, " ")
    for (i = 1; i <= n; i++) {
        comps[arr[i]] = 1
        win_usr[arr[i]] = 0
        win_sys[arr[i]] = 0
        win_cpu[arr[i]] = 0
        win_cnt[arr[i]] = 0
    }
    sample_num = 0
    last_ts    = ""
    window_ts  = ""
}

/^Linux/         { next }
/^[[:space:]]*$/ { next }
/Average/        { next }
/UID/            { next }

NF >= 9 {
    ts   = $1
    comp = $NF
    if (!(comp in comps)) next

    usr = $(NF-6)+0
    sys = $(NF-5)+0
    cpu = $(NF-2)+0

    # Detect boundary between seconds
    if (ts != last_ts) {
        if (last_ts != "") {
            sample_num++
            if (sample_num >= interval) {
                # Flush window: one row per component (mean values)
                for (c in comps) {
                    cnt = (win_cnt[c] > 0) ? win_cnt[c] : 1
                    printf "%s,%.2f,%.2f,%.2f\n", window_ts, \
                        win_usr[c]/cnt, win_sys[c]/cnt, win_cpu[c]/cnt \
                        >> (cpu_dir "/" c ".csv")
                }
                # Reset accumulators
                for (c in comps) { win_usr[c]=0; win_sys[c]=0; win_cpu[c]=0; win_cnt[c]=0 }
                sample_num = 0
            }
        }
        last_ts = ts
        if (sample_num == 0) window_ts = ts   # first second of new window
    }

    # Accumulate sum for mean computation
    win_usr[comp] += usr
    win_sys[comp] += sys
    win_cpu[comp] += cpu
    win_cnt[comp]++
}

END {
    # Flush the last partial window
    if (sample_num > 0)
        for (c in comps) {
            cnt = (win_cnt[c] > 0) ? win_cnt[c] : 1
            printf "%s,%.2f,%.2f,%.2f\n", window_ts, \
                win_usr[c]/cnt, win_sys[c]/cnt, win_cpu[c]/cnt \
                >> (cpu_dir "/" c ".csv")
        }
}
'

# ── awk — RAM metrics (pidstat -r) ───────────────────────────────────────────
#
# pidstat -r column order — 9 fields:
#   $1=Time  $2=UID  $3=PID  $4=minflt/s  $5=majflt/s  $6=VSZ  $7=RSS  $8=%MEM  $9=Command
#
# From the end:
#   $NF     = Command
#   $(NF-1) = %MEM
#   $(NF-2) = RSS  (KB)
#   $(NF-3) = VSZ  (KB)
#   $(NF-4) = majflt/s
#   $(NF-5) = minflt/s

RAM_AWK='
BEGIN {
    n = split(comp_list, arr, " ")
    for (i = 1; i <= n; i++) {
        comps[arr[i]] = 1
        win_minflt[arr[i]] = 0
        win_majflt[arr[i]] = 0
        win_vsz[arr[i]]    = 0
        win_rss[arr[i]]    = 0
        win_mem[arr[i]]    = 0
        win_cnt[arr[i]]    = 0
    }
    sample_num = 0
    last_ts    = ""
    window_ts  = ""
}

/^Linux/         { next }
/^[[:space:]]*$/ { next }
/Average/        { next }
/UID/            { next }

NF >= 8 {
    ts   = $1
    comp = $NF
    if (!(comp in comps)) next

    minflt = $(NF-5)+0
    majflt = $(NF-4)+0
    vsz    = $(NF-3)+0
    rss    = $(NF-2)+0
    mem    = $(NF-1)+0

    if (ts != last_ts) {
        if (last_ts != "") {
            sample_num++
            if (sample_num >= interval) {
                for (c in comps) {
                    cnt = (win_cnt[c] > 0) ? win_cnt[c] : 1
                    printf "%s,%.2f,%.2f,%d,%d,%.2f\n", window_ts, \
                        win_minflt[c]/cnt, win_majflt[c]/cnt, \
                        win_vsz[c]/cnt, win_rss[c]/cnt, win_mem[c]/cnt \
                        >> (ram_dir "/" c ".csv")
                }
                for (c in comps) { win_minflt[c]=0; win_majflt[c]=0; win_vsz[c]=0; win_rss[c]=0; win_mem[c]=0; win_cnt[c]=0 }
                sample_num = 0
            }
        }
        last_ts = ts
        if (sample_num == 0) window_ts = ts
    }

    win_minflt[comp] += minflt
    win_majflt[comp] += majflt
    win_vsz[comp]    += vsz
    win_rss[comp]    += rss
    win_mem[comp]    += mem
    win_cnt[comp]++
}

END {
    if (sample_num > 0)
        for (c in comps) {
            cnt = (win_cnt[c] > 0) ? win_cnt[c] : 1
            printf "%s,%.2f,%.2f,%d,%d,%.2f\n", window_ts, \
                win_minflt[c]/cnt, win_majflt[c]/cnt, \
                win_vsz[c]/cnt, win_rss[c]/cnt, win_mem[c]/cnt \
                >> (ram_dir "/" c ".csv")
        }
}
'

# ── Launch ONE pidstat process per metric, covering ALL components ────────────
#
# A single pidstat run guarantees all components are sampled at the same instant.
# pidstat samples every 1 second for the full DURATION; awk aggregates into
# windows of INTERVAL seconds and writes the max to per-component CSV files.

echo "[pidstat] starting CPU collector  (pidstat -u, 1s samples)..."
pidstat -u 1 "$DURATION" 2>/dev/null \
    | awk \
        -v comp_list="$COMP_STR" \
        -v interval="$INTERVAL"  \
        -v cpu_dir="$CPU_DIR"    \
        "$CPU_AWK" &
CPU_PID=$!

echo "[pidstat] starting RAM collector  (pidstat -r, 1s samples)..."
pidstat -r 1 "$DURATION" 2>/dev/null \
    | awk \
        -v comp_list="$COMP_STR" \
        -v interval="$INTERVAL"  \
        -v ram_dir="$RAM_DIR"    \
        "$RAM_AWK" &
RAM_PID=$!

echo ""
echo "[pidstat] collecting for ${DURATION}s — 1s samples aggregated into ${INTERVAL}s windows (MEAN)."
echo "[pidstat] press Ctrl+C to abort early."
echo ""

wait "$CPU_PID" 2>/dev/null || true
wait "$RAM_PID" 2>/dev/null || true

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "======================================================================"
echo "[pidstat] collection complete."
echo "[pidstat] files written:"
find "$OUT_DIR" -name "*.csv" | sort | while read -r f; do
    LINES=$(wc -l < "$f")
    SAMPLES=$(( LINES - 1 ))
    printf "  %-60s  (%d windows)\n" "$f" "$SAMPLES"
done
echo "======================================================================"
