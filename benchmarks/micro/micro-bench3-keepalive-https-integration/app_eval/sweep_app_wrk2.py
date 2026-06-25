#!/usr/bin/env python3
"""
sweep_app_wrk2.py — Rate sweep wrk2 pour le MACRO-bench BeFaaS IoT.

Identique en esprit à evaluation_throughput/sweep_throughput_wrk2.py (même CSV,
mêmes métriques, même monitoring CPU/pidstat du Pi), mais :
  - la cible est le point d'entrée `objectrecognition` (déclenche la chaîne
    objectrecognition -> {trafficstatistics, emergencydetection -> setlightphasecalculation}) ;
  - la requête est un POST multipart/form-data d'une IMAGE (client/post_image.lua) ;
  - le schéma http|https est sélectionnable (port 8080 vs 8443) ;
  - vanilla et proto déploient le MÊME nom de fonction, donc l'URL est identique
    dans les deux modes : --mode ne sert qu'au label de sortie et au pidstat.

Le CSV produit a EXACTEMENT les colonnes attendues par compare_two_csv_plots.py.

Exemples :
  # Prototype, HTTPS
  python3 sweep_app_wrk2.py --mode proto --scheme https \
      --rates 25,50,75,100,150,200 --concurrency 100 \
      --out results/proto_https_objreco_100c.csv

  # Vanilla, HTTP
  python3 sweep_app_wrk2.py --mode vanilla --scheme http \
      --rates 25,50,75,100,150,200 --concurrency 100 \
      --out results/vanilla_http_objreco_100c.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple

DEFAULT_GATEWAY_IP = "192.168.2.2"
DEFAULT_PI_SSH     = "romero@192.168.2.2"
DEFAULT_FUNCTION   = "objectrecognition"

# Les 4 fonctions du macro-bench (pour la résolution pidstat, vanilla ET proto).
MACRO_FUNCTIONS = [
    "objectrecognition",
    "emergencydetection",
    "trafficstatistics",
    "setlightphasecalculation",
]

_LATENCY_STATS_RE = re.compile(
    r"Latency\s+"
    r"(?P<avg>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<avg_u>us|ms|s)\s+"
    r"(?P<stdev>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<stdev_u>us|ms|s)\s+"
    r"(?P<max>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<max_u>us|ms|s)",
    re.IGNORECASE,
)
_PERCENTILE_RE = re.compile(
    r"(?P<pct>[0-9]+(?:\.[0-9]+)?)%\s+(?P<val>[0-9]+\.?[0-9]*)\s*(?P<unit>us|ms|s)"
)
_REQUESTS_IN_RE = re.compile(
    r"(?P<reqs>[0-9,]+)\s+requests in\s+[0-9.]+s,\s+(?P<read>[0-9.]+)(?P<read_u>KB|MB|GB)\s+read"
)
_RPS_RE = re.compile(r"Requests/sec:\s+(?P<rps>[0-9.]+)")
_TRANSFER_RE = re.compile(r"Transfer/sec:\s+(?P<val>[0-9.]+)(?P<unit>KB|MB|GB)")
_ERRORS_RE = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")
_SOCKET_ERRORS_RE = re.compile(
    r"Socket errors:\s*connect\s+(?P<connect>[0-9]+),\s*read\s+(?P<read>[0-9]+),\s*write\s+(?P<write>[0-9]+),\s*timeout\s+(?P<timeout>[0-9]+)"
)


def _to_ms(value: float, unit: str) -> float:
    if unit == "us":
        return value / 1000.0
    if unit == "ms":
        return value
    if unit == "s":
        return value * 1000.0
    return value


def _to_mb(value: float, unit: str) -> float:
    if unit == "KB":
        return value / 1024.0
    if unit == "MB":
        return value
    if unit == "GB":
        return value * 1024.0
    return value


def _parse_float_or_zero(raw: str | None) -> float:
    if raw is None:
        return 0.0
    lowered = raw.strip().lower()
    if lowered in {"nan", "-nan", "+nan"}:
        return 0.0
    return float(raw)


def _parse_int_list(raw: str) -> List[int]:
    out: List[int] = []
    for chunk in raw.split(","):
        token = chunk.strip()
        if not token:
            continue
        value = int(token)
        if value <= 0:
            raise ValueError(f"rate must be > 0, got: {value}")
        out.append(value)
    return out


def _find_wrk2() -> str:
    env = os.environ.get("WRK2")
    if env:
        expanded = os.path.expanduser(env)
        if os.path.isfile(expanded) and os.access(expanded, os.X_OK):
            return expanded
    for candidate in (os.path.expanduser("~/wrk2/wrk"), os.path.expanduser("~/wrk2/wrk2")):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return "wrk2"


def _parse_wrk2_output(text: str) -> Dict[str, float | int | str]:
    m = _LATENCY_STATS_RE.search(text)
    lat_avg_ms = lat_stdev_ms = lat_max_ms = 0.0
    if m:
        lat_avg_ms = _to_ms(_parse_float_or_zero(m.group("avg")), m.group("avg_u"))
        lat_stdev_ms = _to_ms(_parse_float_or_zero(m.group("stdev")), m.group("stdev_u"))
        lat_max_ms = _to_ms(_parse_float_or_zero(m.group("max")), m.group("max_u"))
    else:
        print("  [WARN] ligne Latency introuvable — sortie brute wrk2 :")
        for line in text.splitlines():
            print(f"    | {line}")

    percentiles: Dict[str, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pct = float(pm.group("pct"))
        percentiles[f"p{pct:g}_ms"] = _to_ms(float(pm.group("val")), pm.group("unit"))

    req_m = _REQUESTS_IN_RE.search(text)
    total_requests = int(req_m.group("reqs").replace(",", "")) if req_m else 0
    read_mb = _to_mb(float(req_m.group("read")), req_m.group("read_u")) if req_m else 0.0

    rps_m = _RPS_RE.search(text)
    rps = float(rps_m.group("rps")) if rps_m else 0.0

    tr_m = _TRANSFER_RE.search(text)
    transfer_mb_s = _to_mb(float(tr_m.group("val")), tr_m.group("unit")) if tr_m else 0.0

    err_m = _ERRORS_RE.search(text)
    errors_non2xx = int(err_m.group("n")) if err_m else 0

    sock_m = _SOCKET_ERRORS_RE.search(text)
    sc = int(sock_m.group("connect")) if sock_m else 0
    sr = int(sock_m.group("read")) if sock_m else 0
    sw = int(sock_m.group("write")) if sock_m else 0
    st = int(sock_m.group("timeout")) if sock_m else 0

    out: Dict[str, float | int | str] = {
        "rps": round(rps, 3),
        "transfer_kb_s": round(transfer_mb_s * 1024.0, 3),
        "transfer_mb_s": round(transfer_mb_s, 3),
        "lat_avg_ms": round(lat_avg_ms, 3),
        "lat_stdev_ms": round(lat_stdev_ms, 3),
        "lat_max_ms": round(lat_max_ms, 3),
        "total_requests": total_requests,
        "read_mb": round(read_mb, 3),
        "errors_non2xx": errors_non2xx,
        "socket_connect_errors": sc,
        "socket_read_errors": sr,
        "socket_write_errors": sw,
        "socket_timeout_errors": st,
    }
    out["p50_ms"] = round(percentiles.get("p50_ms", 0.0), 3)
    out["p75_ms"] = round(percentiles.get("p75_ms", 0.0), 3)
    out["p90_ms"] = round(percentiles.get("p90_ms", 0.0), 3)
    out["p95_ms"] = round(percentiles.get("p95_ms", 0.0), 3)
    out["p99_ms"] = round(percentiles.get("p99_ms", 0.0), 3)
    return out


# ── Pi CPU global (via /proc/stat over ssh) ───────────────────────────────────

def _start_pi_cpu_sampling(pi_ssh: str, samples: int, interval_s: int) -> subprocess.Popen[str]:
    remote_cmd = (
        "nproc; "
        f"for i in $(seq 1 {samples}); do "
        "awk '/^cpu / {print $2,$3,$4,$5,$6,$7,$8,$9}' /proc/stat; "
        f"sleep {interval_s}; "
        "done"
    )
    return subprocess.Popen(["ssh", pi_ssh, remote_cmd],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _compute_cpu_busy_stats(sample_text: str) -> Tuple[float, float, float, int]:
    lines = sample_text.strip().splitlines()
    n_cpus = 1
    data_lines = lines
    if lines:
        try:
            candidate = int(lines[0].strip())
            if candidate > 0:
                n_cpus = candidate
                data_lines = lines[1:]
        except ValueError:
            pass

    rows: List[Tuple[int, ...]] = []
    for raw in data_lines:
        parts = raw.strip().split()
        if len(parts) < 8:
            continue
        try:
            rows.append(tuple(int(x) for x in parts[:8]))
        except ValueError:
            continue
    if len(rows) < 2:
        return 0.0, 0.0, 0.0, len(rows)

    busy_pcts: List[float] = []
    for prev, cur in zip(rows, rows[1:]):
        pu, pn, ps, pidle, piow, pirq, psoft, psteal = prev
        cu, cn, cs, cidle, ciow, cirq, csoft, csteal = cur
        prev_busy = pu + pn + ps + pirq + psoft + psteal
        cur_busy = cu + cn + cs + cirq + csoft + csteal
        prev_total = prev_busy + pidle + piow
        cur_total = cur_busy + cidle + ciow
        d_total = cur_total - prev_total
        d_busy = cur_busy - prev_busy
        if d_total <= 0:
            continue
        busy_pcts.append((100.0 * n_cpus * d_busy) / d_total)
    if not busy_pcts:
        return 0.0, 0.0, 0.0, len(rows)
    return (round(sum(busy_pcts) / len(busy_pcts), 3),
            round(max(busy_pcts), 3), round(min(busy_pcts), 3), len(rows))


# ── pidstat par composant (gateway, faasd, fwatchdog-<fn>, worker-<fn>) ───────
#
# Adapté du sweep micro : on matche les 4 fonctions du macro-bench (mêmes noms
# en vanilla et proto), et le process métier est "node" (node index.js).

_PID_RESOLVE_SCRIPT = r"""
FN_REGEX="$1"
WORKER_COMM="$2"
declare -A PID_LABEL

GW_PID=$(ctr -n openfaas task ls 2>/dev/null | awk '$1=="gateway"{print $2}')
[[ -n "$GW_PID" ]] && PID_LABEL[$GW_PID]="gateway"

for p in $(pgrep -x faasd 2>/dev/null); do
    PID_LABEL[$p]="faasd"
done

while read -r FN FNPID _STATUS; do
    [[ "$FN" == "TASK" || -z "$FN" ]] && continue
    [[ "$_STATUS" != "RUNNING" ]] && continue
    [[ ! "$FN" =~ ^($FN_REGEX)$ ]] && continue
    PID_LABEL[$FNPID]="fwatchdog-${FN}"
    for CPID in $(pgrep -P "$FNPID" 2>/dev/null); do
        CCOMM=$(ps -p "$CPID" -o comm= 2>/dev/null || true)
        [[ "$CCOMM" == "$WORKER_COMM" ]] && PID_LABEL[$CPID]="worker-${FN}"
    done
done < <(ctr -n openfaas-fn task ls 2>/dev/null)

PARTS=()
for pid in "${!PID_LABEL[@]}"; do
    PARTS+=("${pid}:${PID_LABEL[$pid]}")
done
IFS=,
echo "${PARTS[*]}"
"""


def _resolve_pidstat_pids(pi_ssh: str) -> Dict[str, str]:
    fn_regex = "|".join(MACRO_FUNCTIONS)
    remote_cmd = f"sudo bash -s -- {shlex.quote(fn_regex)} {shlex.quote('node')}"
    result = subprocess.run(["ssh", pi_ssh, remote_cmd], input=_PID_RESOLVE_SCRIPT,
                            capture_output=True, text=True, timeout=30)
    raw = result.stdout.strip()
    pid_label: Dict[str, str] = {}
    if raw:
        for pair in raw.split(","):
            pid, _, label = pair.partition(":")
            if pid and label:
                pid_label[pid] = label
    return pid_label


def _start_pi_pidstat_sampling(pi_ssh: str, pid_csv: str, duration_s: int):
    cpu_cmd = f"sudo LC_ALL=C pidstat -u -p {pid_csv} 1 {duration_s} 2>/dev/null"
    ram_cmd = f"sudo LC_ALL=C pidstat -r -p {pid_csv} 1 {duration_s} 2>/dev/null"
    cpu_proc = subprocess.Popen(["ssh", pi_ssh, cpu_cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    ram_proc = subprocess.Popen(["ssh", pi_ssh, ram_cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return cpu_proc, ram_proc


def _percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def _parse_pidstat_per_second(text: str, pid_label: Dict[str, str], offsets: Dict[str, int]):
    labels = sorted(set(pid_label.values()))
    samples = {label: {metric: [] for metric in offsets} for label in labels}
    cur = {label: {metric: 0.0 for metric in offsets} for label in labels}
    last_ts = None
    have_second = False

    def flush_second() -> None:
        for label in labels:
            for metric in offsets:
                samples[label][metric].append(cur[label][metric])
                cur[label][metric] = 0.0

    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("Linux") or "Average" in line or "UID" in line:
            continue
        parts = line.split()
        if len(parts) < 8:
            continue
        ts, pid = parts[0], parts[2]
        if pid not in pid_label:
            continue
        label = pid_label[pid]
        if ts != last_ts:
            if have_second:
                flush_second()
            last_ts = ts
            have_second = True
        for metric, neg_idx in offsets.items():
            try:
                cur[label][metric] += float(parts[neg_idx])
            except (IndexError, ValueError):
                pass
    if have_second:
        flush_second()
    return samples


_CPU_OFFSETS = {"usr_pct": -7, "system_pct": -6, "cpu_pct": -3}
_RAM_OFFSETS = {"minflt_s": -6, "majflt_s": -5, "vsz_kb": -4, "rss_kb": -3, "mem_pct": -2}


def _pidstat_window_p75(text: str, pid_label: Dict[str, str], offsets: Dict[str, int]):
    per_second = _parse_pidstat_per_second(text, pid_label, offsets)
    return {label: {metric: round(_percentile(vals, 75.0), 2) for metric, vals in metrics.items()}
            for label, metrics in per_second.items()}


# ── wrk2 ──────────────────────────────────────────────────────────────────────

def _run_wrk2(wrk2_bin, lua_script, url, req_path, image_path, duration_s, timeout_s,
              threads, concurrency, rate) -> Tuple[int, str]:
    env = os.environ.copy()
    env["WRK_IMAGE_PATH"] = image_path
    env["WRK_PATH"] = req_path
    actual_threads = min(threads, concurrency)
    cmd = [wrk2_bin, f"-t{actual_threads}", f"-c{concurrency}", f"-d{duration_s}s",
           f"-R{rate}", "--timeout", f"{timeout_s}s", "--latency", "-s", lua_script, url]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, env=env,
                                timeout=duration_s + timeout_s + 40)
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 binary not found"
    output = (result.stdout or "") + ("\n" if result.stdout and result.stderr else "") + (result.stderr or "")
    return result.returncode, output


def _base_row() -> Dict[str, object]:
    return {
        "timestamp": "", "sweep_type": "rate_sweep", "monitored_variable": "rate",
        "monitored_value": 0, "rate": 0, "concurrency": 0, "target_mode": "",
        "payload_kb": 0, "exit_code": 0, "rps": 0.0, "transfer_kb_s": 0.0,
        "transfer_mb_s": 0.0, "lat_avg_ms": 0.0, "lat_stdev_ms": 0.0, "lat_max_ms": 0.0,
        "p50_ms": 0.0, "p75_ms": 0.0, "p90_ms": 0.0, "p95_ms": 0.0, "p99_ms": 0.0,
        "total_requests": 0, "read_mb": 0.0, "errors_non2xx": 0,
        "socket_connect_errors": 0, "socket_read_errors": 0, "socket_write_errors": 0,
        "socket_timeout_errors": 0, "pi_cpu_busy_avg_pct": 0.0, "pi_cpu_busy_max_pct": 0.0,
        "pi_cpu_busy_min_pct": 0.0, "pi_cpu_samples": 0,
    }


def main():
    p = argparse.ArgumentParser(description="Rate sweep wrk2 — macro BeFaaS IoT (objectrecognition)")
    p.add_argument("--mode", choices=["vanilla", "proto"], required=True,
                   help="Label de sortie + résolution pidstat (l'URL est identique)")
    p.add_argument("--scheme", choices=["http", "https"], required=True, help="http (8080) ou https (8443)")
    p.add_argument("--gateway-ip", default=DEFAULT_GATEWAY_IP, help="IP du gateway (def 192.168.2.2)")
    p.add_argument("--function", default=DEFAULT_FUNCTION, help="Fonction d'entrée (def objectrecognition)")
    p.add_argument("--rates", default="5,10,15,20,25,30,40,50",
                   help="Débits (req/s). Plage basse: le macro-bench (jimp + ~4 handshakes TLS + ~10 ops Redis par requête) sature le Pi à bas RPS.")
    p.add_argument("--concurrency", type=int, default=8,
                   help="Connexions concurrentes. BAS pour le macro (requêtes lourdes: jimp + 4 handshakes TLS + Redis). c=100 noie le Pi -> timeouts/500.")
    p.add_argument("--image", default="images/image-ambulance.jpg",
                   help="Image multipart envoyée (ambulance=chemin rapide/emergency)")
    p.add_argument("--out", required=True, help="CSV de sortie")
    p.add_argument("--pi-ssh", default=DEFAULT_PI_SSH, help="SSH du Pi pour CPU/pidstat")
    p.add_argument("--duration-s", type=int, default=20, help="Durée de chaque palier")
    p.add_argument("--timeout-s", type=int, default=15, help="Timeout requête wrk2")
    p.add_argument("--threads", type=int, default=4, help="Threads client wrk2")
    p.add_argument("--pause", type=int, default=5, help="Pause entre paliers")
    p.add_argument("--no-pidstat", action="store_true", help="Désactive le pidstat par composant")
    args = p.parse_args()

    rates = _parse_int_list(args.rates)
    wrk2_bin = _find_wrk2()
    script_dir = Path(__file__).resolve().parent
    lua_script = str(script_dir / "client" / "post_image.lua")

    # Image : résolue par rapport au dossier du script si chemin relatif.
    image_path = args.image
    if not os.path.isabs(image_path):
        image_path = str(script_dir / image_path)
    if not os.path.isfile(image_path):
        print(f"ERREUR: image introuvable: {image_path}", file=sys.stderr)
        return 1
    payload_kb = max(1, round(os.path.getsize(image_path) / 1024.0))

    port = 8443 if args.scheme == "https" else 8080
    req_path = f"/function/{args.function}"
    url = f"{args.scheme}://{args.gateway_ip}:{port}{req_path}"

    print(f"=== Macro sweep wrk2 ({args.mode.upper()} / {args.scheme.upper()}) ===")
    print(f"URL        : {url}")
    print(f"Image      : {image_path} (~{payload_kb} KB)")
    print(f"Rates      : {rates}")
    print(f"Concurrency: {args.concurrency}")
    print(f"wrk2 Bin   : {wrk2_bin}")
    print(f"Out CSV    : {args.out}\n")

    pidstat_enabled = not args.no_pidstat
    pid_label: Dict[str, str] = {}
    pid_csv = ""
    pidstat_mode_dir = "vanilla" if args.mode == "vanilla" else "prototype"
    pidstat_cpu_rows: Dict[str, List[Dict[str, object]]] = {}
    pidstat_ram_rows: Dict[str, List[Dict[str, object]]] = {}

    if pidstat_enabled:
        print("Résolution des PID pidstat sur le Pi...")
        pid_label = _resolve_pidstat_pids(args.pi_ssh)
        if not pid_label:
            print("  [WARN] aucun PID résolu — pidstat désactivé pour ce run.")
            pidstat_enabled = False
        else:
            pid_csv = ",".join(sorted(pid_label.keys(), key=int))
            for label in sorted(set(pid_label.values())):
                pidstat_cpu_rows[label] = []
                pidstat_ram_rows[label] = []
            print(f"  composants: {', '.join(sorted(set(pid_label.values())))}\n")

    rows = []
    steps_with_server_errors: List[int] = []
    for idx, rate in enumerate(rates, start=1):
        print(f"[{idx}/{len(rates)}] rate={rate} (C={args.concurrency}, img~{payload_kb}KB)...")

        cpu_samples = max(3, args.duration_s + 2)
        cpu_proc = _start_pi_cpu_sampling(args.pi_ssh, cpu_samples, 1)

        pidstat_cpu_proc = pidstat_ram_proc = None
        if pidstat_enabled:
            pidstat_cpu_proc, pidstat_ram_proc = _start_pi_pidstat_sampling(args.pi_ssh, pid_csv, args.duration_s)

        exit_code, output = _run_wrk2(wrk2_bin, lua_script, url, req_path, image_path,
                                      args.duration_s, args.timeout_s, args.threads,
                                      args.concurrency, rate)

        try:
            cpu_stdout, _ = cpu_proc.communicate(timeout=args.duration_s + args.timeout_s + 30)
        except subprocess.TimeoutExpired:
            cpu_proc.kill()
            cpu_stdout, _ = cpu_proc.communicate()
        cpu_avg, cpu_max, cpu_min, cpu_count = _compute_cpu_busy_stats(cpu_stdout)

        if pidstat_enabled and pidstat_cpu_proc and pidstat_ram_proc:
            try:
                pcpu, _ = pidstat_cpu_proc.communicate(timeout=args.duration_s + 30)
            except subprocess.TimeoutExpired:
                pidstat_cpu_proc.kill(); pcpu, _ = pidstat_cpu_proc.communicate()
            try:
                pram, _ = pidstat_ram_proc.communicate(timeout=args.duration_s + 30)
            except subprocess.TimeoutExpired:
                pidstat_ram_proc.kill(); pram, _ = pidstat_ram_proc.communicate()
            cpu_p75 = _pidstat_window_p75(pcpu, pid_label, _CPU_OFFSETS)
            ram_p75 = _pidstat_window_p75(pram, pid_label, _RAM_OFFSETS)
            for label in pidstat_cpu_rows:
                cr = {"rate": rate}; cr.update(cpu_p75.get(label, {m: 0.0 for m in _CPU_OFFSETS}))
                pidstat_cpu_rows[label].append(cr)
                rr = {"rate": rate}; rr.update(ram_p75.get(label, {m: 0.0 for m in _RAM_OFFSETS}))
                rr["vsz_kb"] = int(round(rr["vsz_kb"])); rr["rss_kb"] = int(round(rr["rss_kb"]))
                pidstat_ram_rows[label].append(rr)

        parsed = _parse_wrk2_output(output)
        row = _base_row()
        row.update(parsed)
        row["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
        row["monitored_value"] = rate
        row["rate"] = rate
        row["concurrency"] = args.concurrency
        row["target_mode"] = f"{args.mode}-{args.scheme}"
        row["payload_kb"] = payload_kb
        row["exit_code"] = exit_code
        row["pi_cpu_busy_avg_pct"] = cpu_avg
        row["pi_cpu_busy_max_pct"] = cpu_max
        row["pi_cpu_busy_min_pct"] = cpu_min
        row["pi_cpu_samples"] = cpu_count
        rows.append(row)

        server_errors = (int(row["errors_non2xx"]) + int(row["socket_connect_errors"])
                         + int(row["socket_read_errors"]) + int(row["socket_write_errors"]))
        print(f"  RPS={row['rps']:7.2f} | AvgLat={row['lat_avg_ms']:7.2f} ms | p99={row['p99_ms']:7.2f} ms\n"
              f"  Pi CPU avg={row['pi_cpu_busy_avg_pct']:6.2f}% max={row['pi_cpu_busy_max_pct']:6.2f}%\n"
              f"  Errors: non2xx={row['errors_non2xx']} connect={row['socket_connect_errors']} "
              f"read={row['socket_read_errors']} write={row['socket_write_errors']} timeout={row['socket_timeout_errors']}")
        if exit_code != 0:
            print(f"  [WARN] wrk2 exit code {exit_code}")
        if server_errors > 0:
            steps_with_server_errors.append(rate)
            print(f"  [WARN] {server_errors} erreurs serveur à rate={rate} — données peu fiables "
                  f"(vérifier: sudo ctr -n openfaas-fn task ls)")
        print()

        if idx < len(rates) and args.pause > 0:
            time.sleep(args.pause)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"[ok] CSV écrit: {out_path}")

    if steps_with_server_errors:
        print(f"[WARNING] erreurs serveur aux rates: {', '.join(map(str, steps_with_server_errors))}")

    if pidstat_enabled:
        pidstat_dir = script_dir / "pidstat" / pidstat_mode_dir
        cpu_dir = pidstat_dir / "cpu"; ram_dir = pidstat_dir / "ram"
        cpu_dir.mkdir(parents=True, exist_ok=True); ram_dir.mkdir(parents=True, exist_ok=True)
        for label, cpu_rows in pidstat_cpu_rows.items():
            with (cpu_dir / f"{label}.csv").open("w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=["rate", "usr_pct", "system_pct", "cpu_pct"])
                w.writeheader(); w.writerows(cpu_rows)
        for label, ram_rows in pidstat_ram_rows.items():
            with (ram_dir / f"{label}.csv").open("w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=["rate", "minflt_s", "majflt_s", "vsz_kb", "rss_kb", "mem_pct"])
                w.writeheader(); w.writerows(ram_rows)
        print(f"[ok] pidstat par composant: {pidstat_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
