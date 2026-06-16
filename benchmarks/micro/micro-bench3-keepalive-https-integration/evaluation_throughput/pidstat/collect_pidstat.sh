#!/bin/bash
# collect_pidstat.sh
#
# Collects per-component CPU and RAM metrics using pidstat during throughput evaluation.
#
# KEY DESIGN:
#   - fn-a and fn-b run concurrently. Their fwatchdog/worker processes share the
#     EXACT SAME Linux comm string (15-char truncated name), so they cannot be
#     told apart by name alone. Before starting pidstat, this script resolves
#     the actual PID of each component instance via `ctr task ls` (fwatchdog)
#     and its child process(es) (the worker), and labels each PID individually:
#       fwatchdog-<function>, worker-<function>, gateway, faasd
#   - ONE pidstat process (restricted to those resolved PIDs via -p) monitors
#     everything simultaneously → perfect time alignment, no unrelated processes.
#   - Within a second, PIDs that map to the SAME label (e.g. faasd's several
#     processes, or a function's two worker children) are SUMMED — this is the
#     correct combined load of that one logical component. PIDs belonging to
#     DIFFERENT functions never share a label, so fn-a and fn-b are always kept
#     separate even though their processes have identical comm strings.
#   - Those per-second totals are then aggregated into windows of --interval
#     seconds, keeping the MEAN.
#   - awk writes directly to per-component CSV files: all files have the same
#     row count and line N in gateway.csv corresponds exactly to line N in
#     fwatchdog-<fn>.csv, etc.
#
# Usage (must run as root — needed for `ctr`):
#   sudo ./collect_pidstat.sh --mode <vanilla|prototype> \
#                             --duration <seconds>        \
#                             --interval <seconds>
#
#   --mode      vanilla   : gateway, faasd, fwatchdog-<fn>, worker-<fn>  (fn = vanilla-fn-a/b)
#               prototype : gateway, faasd, fwatchdog-<fn>, worker-<fn>  (fn = sumprod-timing-fn-a/b, ...)
#   --duration  total collection time in seconds (= number of wrk2 rate steps × step duration)
#   --interval  aggregation window in seconds    (= your wrk2 window / step duration)
#
# Output:
#   pidstat/<mode>/cpu/<component>.csv   → timestamp, usr_pct, system_pct, cpu_pct
#   pidstat/<mode>/ram/<component>.csv   → timestamp, minflt_s, majflt_s, vsz_kb, rss_kb, mem_pct

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Error: this script needs root (it calls 'ctr' to resolve container PIDs). Re-run with sudo." >&2
    exit 1
fi

# ── Argument parsing ──────────────────────────────────────────────────────────

usage() {
    echo "Usage: sudo $0 --mode <vanilla|prototype> --duration <seconds> --interval <seconds>"
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

# ── Resolve PID → label for every component instance ──────────────────────────
#
# gateway/faasd are host-level singletons (faasd may legitimately have several
# processes — provider, up, one shim per container — all summed under "faasd").
# Each function's fwatchdog PID comes from `ctr task ls`; its worker child PID(s)
# come from `pgrep -P <fwatchdog_pid>`, filtered to the expected worker comm so
# unrelated children are never picked up.

declare -A PID_LABEL

GW_PID=$(ctr -n openfaas task ls 2>/dev/null | awk '$1=="gateway"{print $2}')
[[ -n "$GW_PID" ]] && PID_LABEL[$GW_PID]="gateway"

for p in $(pgrep -x faasd 2>/dev/null); do
    PID_LABEL[$p]="faasd"
done

if [[ "$MODE" == "vanilla" ]]; then
    WORKER_COMM="vanilla-fn-work"
else
    WORKER_COMM="timing-fn-ka-wo"
fi

FN_MATCHES=0
while read -r FN FNPID _STATUS; do
    [[ "$FN" == "TASK" || -z "$FN" ]] && continue
    if [[ "$MODE" == "vanilla" ]]; then
        [[ "$FN" != *vanilla* ]] && continue
    else
        [[ "$FN" == *vanilla* ]] && continue
    fi
    FN_MATCHES=$((FN_MATCHES + 1))
    PID_LABEL[$FNPID]="fwatchdog-${FN}"
    for CPID in $(pgrep -P "$FNPID" 2>/dev/null); do
        CCOMM=$(ps -p "$CPID" -o comm= 2>/dev/null || true)
        [[ "$CCOMM" == "$WORKER_COMM" ]] && PID_LABEL[$CPID]="worker-${FN}"
    done
done < <(ctr -n openfaas-fn task ls 2>/dev/null)

if [[ $FN_MATCHES -eq 0 ]]; then
    echo "[pidstat] WARNING: no running function task matched mode='$MODE' — only gateway/faasd will be measured. Did you deploy the functions?" >&2
fi

if [[ ${#PID_LABEL[@]} -eq 0 ]]; then
    echo "Error: resolved zero PIDs (gateway/faasd not found). Is faasd running?" >&2
    exit 1
fi

PID_LIST=()
PID_MAP_PARTS=()
for pid in "${!PID_LABEL[@]}"; do
    PID_LIST+=("$pid")
    PID_MAP_PARTS+=("${pid}:${PID_LABEL[$pid]}")
done
PID_CSV=$(IFS=,; echo "${PID_LIST[*]}")
PID_MAP_STR=$(IFS=,; echo "${PID_MAP_PARTS[*]}")

mapfile -t LABELS < <(printf '%s\n' "${PID_LABEL[@]}" | sort -u)

echo "[pidstat] resolved components:"
for COMP in "${LABELS[@]}"; do
    COMP_PIDS=()
    for pid in "${!PID_LABEL[@]}"; do
        [[ "${PID_LABEL[$pid]}" == "$COMP" ]] && COMP_PIDS+=("$pid")
    done
    printf "  %-30s pid(s): %s\n" "$COMP" "${COMP_PIDS[*]}"
done

# ── Output directories ────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/$MODE"
CPU_DIR="$OUT_DIR/cpu"
RAM_DIR="$OUT_DIR/ram"

mkdir -p "$CPU_DIR" "$RAM_DIR"

# ── Write CSV headers (overwrites any previous run) ───────────────────────────

for COMP in "${LABELS[@]}"; do
    printf "timestamp,usr_pct,system_pct,cpu_pct\n"               > "$CPU_DIR/${COMP}.csv"
    printf "timestamp,minflt_s,majflt_s,vsz_kb,rss_kb,mem_pct\n" > "$RAM_DIR/${COMP}.csv"
done

echo "======================================================================"
echo "[pidstat] mode=$MODE  duration=${DURATION}s  interval=${INTERVAL}s  (MEAN per window, summed only across PIDs of the SAME component)"
echo "[pidstat] output: $OUT_DIR"
echo "======================================================================"

# ── awk — CPU metrics (pidstat -u) ───────────────────────────────────────────
#
# pidstat -u column order (sysstat 12.x, aarch64) — 10 fields:
#   $1=Time  $2=UID  $3=PID  $4=%usr  $5=%system  $6=%guest  $7=%wait  $8=%CPU  $9=CPU  $10=Command
#
# Component identity comes from PID (via pid_map), NOT from the Command name —
# this is what lets fn-a's and fn-b's identically-named fwatchdog/worker stay
# separate. We still read %usr/%system/%CPU from the END so the script stays
# robust to sysstat versions that add/remove columns in the middle:
#   $(NF-2) = %CPU   (total cpu%)
#   $(NF-5) = %system
#   $(NF-6) = %usr
#
# WINDOW LOGIC:
#   - A new second is detected when the timestamp ($1) changes.
#   - Within a second, PIDs mapping to the SAME label are SUMMED into that
#     second's total for that label.
#   - Each finished second's total becomes one sample of the current window.
#   - When the window reaches INTERVAL seconds, flush: write the MEAN of
#     those per-second totals (one row per component). A component with no
#     PID active in a given second contributes 0 for that second, which
#     guarantees every CSV file has the same row count.

CPU_AWK='
function mean(arr, n,    sum, i) {
    sum = 0
    for (i = 1; i <= n; i++) sum += arr[i]
    return sum / n
}

BEGIN {
    nm = split(pid_map, pairs, ",")
    for (i = 1; i <= nm; i++) {
        split(pairs[i], kv, ":")
        label_of[kv[1]] = kv[2]
        comps[kv[2]] = 1
    }
    sec_count   = 0
    last_ts     = ""
    window_ts   = ""
    have_second = 0
}

/^Linux/         { next }
/^[[:space:]]*$/ { next }
/Average/        { next }
/UID/            { next }

NF >= 9 {
    ts  = $1
    pid = $3
    if (!(pid in label_of)) next
    comp = label_of[pid]

    usr = $(NF-6)+0
    sys = $(NF-5)+0
    cpu = $(NF-2)+0

    if (ts != last_ts) {
        if (have_second) {
            sec_count++
            for (c in comps) {
                samp_usr[c, sec_count] = cur_usr[c]
                samp_sys[c, sec_count] = cur_sys[c]
                samp_cpu[c, sec_count] = cur_cpu[c]
            }
            if (sec_count >= interval) {
                for (c in comps) {
                    for (k = 1; k <= sec_count; k++) {
                        tmp_usr[k] = samp_usr[c, k]
                        tmp_sys[k] = samp_sys[c, k]
                        tmp_cpu[k] = samp_cpu[c, k]
                    }
                    printf "%s,%.2f,%.2f,%.2f\n", window_ts, \
                        mean(tmp_usr, sec_count), mean(tmp_sys, sec_count), mean(tmp_cpu, sec_count) \
                        >> (cpu_dir "/" c ".csv")
                }
                sec_count = 0
            }
        }
        last_ts = ts
        if (sec_count == 0) window_ts = ts   # first second of new window
        for (c in comps) { cur_usr[c] = 0; cur_sys[c] = 0; cur_cpu[c] = 0 }
        have_second = 1
    }

    # Sum across PIDs that map to this same label within the same second.
    cur_usr[comp] += usr
    cur_sys[comp] += sys
    cur_cpu[comp] += cpu
}

END {
    if (have_second) {
        sec_count++
        for (c in comps) {
            samp_usr[c, sec_count] = cur_usr[c]
            samp_sys[c, sec_count] = cur_sys[c]
            samp_cpu[c, sec_count] = cur_cpu[c]
        }
    }
    if (sec_count > 0)
        for (c in comps) {
            for (k = 1; k <= sec_count; k++) {
                tmp_usr[k] = samp_usr[c, k]
                tmp_sys[k] = samp_sys[c, k]
                tmp_cpu[k] = samp_cpu[c, k]
            }
            printf "%s,%.2f,%.2f,%.2f\n", window_ts, \
                mean(tmp_usr, sec_count), mean(tmp_sys, sec_count), mean(tmp_cpu, sec_count) \
                >> (cpu_dir "/" c ".csv")
        }
}
'

# ── awk — RAM metrics (pidstat -r) ───────────────────────────────────────────
#
# pidstat -r column order — 9 fields:
#   $1=Time  $2=UID  $3=PID  $4=minflt/s  $5=majflt/s  $6=VSZ  $7=RSS  $8=%MEM  $9=Command
#
# Same PID-based identity as the CPU awk above. From the end:
#   $(NF-1) = %MEM
#   $(NF-2) = RSS  (KB)
#   $(NF-3) = VSZ  (KB)
#   $(NF-4) = majflt/s
#   $(NF-5) = minflt/s

RAM_AWK='
function mean(arr, n,    sum, i) {
    sum = 0
    for (i = 1; i <= n; i++) sum += arr[i]
    return sum / n
}

BEGIN {
    nm = split(pid_map, pairs, ",")
    for (i = 1; i <= nm; i++) {
        split(pairs[i], kv, ":")
        label_of[kv[1]] = kv[2]
        comps[kv[2]] = 1
    }
    sec_count   = 0
    last_ts     = ""
    window_ts   = ""
    have_second = 0
}

/^Linux/         { next }
/^[[:space:]]*$/ { next }
/Average/        { next }
/UID/            { next }

NF >= 8 {
    ts  = $1
    pid = $3
    if (!(pid in label_of)) next
    comp = label_of[pid]

    minflt = $(NF-5)+0
    majflt = $(NF-4)+0
    vsz    = $(NF-3)+0
    rss    = $(NF-2)+0
    mem    = $(NF-1)+0

    if (ts != last_ts) {
        if (have_second) {
            sec_count++
            for (c in comps) {
                samp_minflt[c, sec_count] = cur_minflt[c]
                samp_majflt[c, sec_count] = cur_majflt[c]
                samp_vsz[c, sec_count]    = cur_vsz[c]
                samp_rss[c, sec_count]    = cur_rss[c]
                samp_mem[c, sec_count]    = cur_mem[c]
            }
            if (sec_count >= interval) {
                for (c in comps) {
                    for (k = 1; k <= sec_count; k++) {
                        tmp_minflt[k] = samp_minflt[c, k]
                        tmp_majflt[k] = samp_majflt[c, k]
                        tmp_vsz[k]    = samp_vsz[c, k]
                        tmp_rss[k]    = samp_rss[c, k]
                        tmp_mem[k]    = samp_mem[c, k]
                    }
                    printf "%s,%.2f,%.2f,%d,%d,%.2f\n", window_ts, \
                        mean(tmp_minflt, sec_count), mean(tmp_majflt, sec_count), \
                        mean(tmp_vsz, sec_count), mean(tmp_rss, sec_count), mean(tmp_mem, sec_count) \
                        >> (ram_dir "/" c ".csv")
                }
                sec_count = 0
            }
        }
        last_ts = ts
        if (sec_count == 0) window_ts = ts
        for (c in comps) { cur_minflt[c] = 0; cur_majflt[c] = 0; cur_vsz[c] = 0; cur_rss[c] = 0; cur_mem[c] = 0 }
        have_second = 1
    }

    # Sum across PIDs that map to this same label within the same second.
    cur_minflt[comp] += minflt
    cur_majflt[comp] += majflt
    cur_vsz[comp]    += vsz
    cur_rss[comp]    += rss
    cur_mem[comp]    += mem
}

END {
    if (have_second) {
        sec_count++
        for (c in comps) {
            samp_minflt[c, sec_count] = cur_minflt[c]
            samp_majflt[c, sec_count] = cur_majflt[c]
            samp_vsz[c, sec_count]    = cur_vsz[c]
            samp_rss[c, sec_count]    = cur_rss[c]
            samp_mem[c, sec_count]    = cur_mem[c]
        }
    }
    if (sec_count > 0)
        for (c in comps) {
            for (k = 1; k <= sec_count; k++) {
                tmp_minflt[k] = samp_minflt[c, k]
                tmp_majflt[k] = samp_majflt[c, k]
                tmp_vsz[k]    = samp_vsz[c, k]
                tmp_rss[k]    = samp_rss[c, k]
                tmp_mem[k]    = samp_mem[c, k]
            }
            printf "%s,%.2f,%.2f,%d,%d,%.2f\n", window_ts, \
                mean(tmp_minflt, sec_count), mean(tmp_majflt, sec_count), \
                mean(tmp_vsz, sec_count), mean(tmp_rss, sec_count), mean(tmp_mem, sec_count) \
                >> (ram_dir "/" c ".csv")
        }
}
'

# ── Launch ONE pidstat process per metric, restricted to the resolved PIDs ────

echo "[pidstat] starting CPU collector  (pidstat -u -p <resolved PIDs>, 1s samples)..."
pidstat -u -p "$PID_CSV" 1 "$DURATION" 2>/dev/null \
    | awk \
        -v pid_map="$PID_MAP_STR" \
        -v interval="$INTERVAL"   \
        -v cpu_dir="$CPU_DIR"     \
        "$CPU_AWK" &
CPU_PID=$!

echo "[pidstat] starting RAM collector  (pidstat -r -p <resolved PIDs>, 1s samples)..."
pidstat -r -p "$PID_CSV" 1 "$DURATION" 2>/dev/null \
    | awk \
        -v pid_map="$PID_MAP_STR" \
        -v interval="$INTERVAL"   \
        -v ram_dir="$RAM_DIR"     \
        "$RAM_AWK" &
RAM_PID=$!

echo ""
echo "[pidstat] collecting for ${DURATION}s — 1s samples (summed per component) aggregated into ${INTERVAL}s windows (MEAN)."
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
