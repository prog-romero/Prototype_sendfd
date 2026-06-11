#!/usr/bin/env python3
"""
sweep_throughput_wrk2.py — Throughput rate sweep using wrk2 and Pi CPU monitoring.

Usage:
    python3 sweep_throughput_wrk2.py --mode <vanilla|proto> --rates 50,100,150,200 \
        --concurrency 100 --payload-kb 32 --out results/vanilla_rate_sweep_32kb.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple

# Default target URLs on unified HTTPS gateway port 8443
DEFAULT_VANILLA_URL = "https://192.168.2.2:8443/function/vanilla-fn-a"
DEFAULT_PROTO_URL   = "https://192.168.2.2:8443/function/sumprod-timing-fn-a"
DEFAULT_PI_SSH      = "romero@192.168.2.2"

_LATENCY_STATS_RE = re.compile(
    r"Latency\s+"
    r"(?P<avg>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<avg_u>us|ms|s)\s+"
    r"(?P<stdev>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<stdev_u>us|ms|s)\s+"
    r"(?P<max>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<max_u>us|ms|s)"
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
    if not raw.strip():
        return []
    out: List[int] = []
    for chunk in raw.split(","):
        token = chunk.strip()
        if not token:
            continue
        try:
            value = int(token)
        except ValueError as exc:
            raise ValueError(f"Invalid integer in list: {token}") from exc
        if value <= 0:
            raise ValueError(f"List values must be > 0, got: {value}")
        out.append(value)
    return out


def _find_wrk2() -> str:
    env = os.environ.get("WRK2")
    if env:
        expanded = os.path.expanduser(env)
        if os.path.isfile(expanded) and os.access(expanded, os.X_OK):
            return expanded

    candidates = [
        os.path.expanduser("~/wrk2/wrk"),
        os.path.expanduser("~/wrk2/wrk2"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate

    return "wrk2"


def _parse_wrk2_output(text: str) -> Dict[str, float | int | str]:
    m = _LATENCY_STATS_RE.search(text)

    lat_avg_ms = 0.0
    lat_stdev_ms = 0.0
    lat_max_ms = 0.0
    if m:
        lat_avg_ms = _to_ms(_parse_float_or_zero(m.group("avg")), m.group("avg_u"))
        lat_stdev_ms = _to_ms(_parse_float_or_zero(m.group("stdev")), m.group("stdev_u"))
        lat_max_ms = _to_ms(_parse_float_or_zero(m.group("max")), m.group("max_u"))

    percentiles: Dict[str, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pct = float(pm.group("pct"))
        ms = _to_ms(float(pm.group("val")), pm.group("unit"))
        percentiles[f"p{pct:g}_ms"] = ms

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
    socket_connect = int(sock_m.group("connect")) if sock_m else 0
    socket_read = int(sock_m.group("read")) if sock_m else 0
    socket_write = int(sock_m.group("write")) if sock_m else 0
    socket_timeout = int(sock_m.group("timeout")) if sock_m else 0

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
        "socket_connect_errors": socket_connect,
        "socket_read_errors": socket_read,
        "socket_write_errors": socket_write,
        "socket_timeout_errors": socket_timeout,
    }

    out["p50_ms"] = round(percentiles.get("p50_ms", 0.0), 3)
    out["p75_ms"] = round(percentiles.get("p75_ms", 0.0), 3)
    out["p90_ms"] = round(percentiles.get("p90_ms", 0.0), 3)
    out["p95_ms"] = round(percentiles.get("p95_ms", 0.0), 3)
    out["p99_ms"] = round(percentiles.get("p99_ms", 0.0), 3)

    return out


def _start_pi_cpu_sampling(pi_ssh: str, samples: int, interval_s: int) -> subprocess.Popen[str]:
    remote_cmd = (
        "nproc; "
        f"for i in $(seq 1 {samples}); do "
        "awk '/^cpu / {print $2,$3,$4,$5,$6,$7,$8,$9}' /proc/stat; "
        f"sleep {interval_s}; "
        "done"
    )
    return subprocess.Popen(
        ["ssh", pi_ssh, remote_cmd],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


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

    rows: List[Tuple[int, int, int, int, int, int, int, int]] = []
    for raw in data_lines:
        parts = raw.strip().split()
        if len(parts) < 8:
            continue
        try:
            row = tuple(int(x) for x in parts[:8])
        except ValueError:
            continue
        rows.append(row)

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
        # Scale to match historical CPU metrics (e.g. 4 busy cores -> 400%)
        busy_pcts.append((100.0 * n_cpus * d_busy) / d_total)

    if not busy_pcts:
        return 0.0, 0.0, 0.0, len(rows)

    avg_pct = sum(busy_pcts) / len(busy_pcts)
    return round(avg_pct, 3), round(max(busy_pcts), 3), round(min(busy_pcts), 3), len(rows)


def _run_wrk2(
    wrk2_bin: str,
    lua_script: str,
    url: str,
    duration_s: int,
    timeout_s: int,
    threads: int,
    concurrency: int,
    rate: int,
    payload_kb: int,
    target_mode: str,
    fn_a: str,
    fn_b: str,
) -> Tuple[int, str]:
    env = os.environ.copy()
    env["WRK_PAYLOAD_KB"] = str(payload_kb)
    env["WRK_TARGET_MODE"] = target_mode
    env["WRK_SAME_TARGET"] = fn_a
    env["WRK_FN_A"] = fn_a
    env["WRK_FN_B"] = fn_b

    actual_threads = min(threads, concurrency)

    cmd = [
        wrk2_bin,
        f"-t{actual_threads}",
        f"-c{concurrency}",
        f"-d{duration_s}s",
        f"-R{rate}",
        "--timeout",
        f"{timeout_s}s",
        "--latency",
        "-s",
        lua_script,
        url,
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            timeout=duration_s + timeout_s + 40,
        )
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 binary not found"

    output = (result.stdout or "") + ("\n" if result.stdout and result.stderr else "") + (result.stderr or "")
    return result.returncode, output


def _base_row() -> Dict[str, object]:
    return {
        "timestamp": "",
        "sweep_type": "rate_sweep",
        "monitored_variable": "rate",
        "monitored_value": 0,
        "rate": 0,
        "concurrency": 0,
        "target_mode": "",
        "payload_kb": 0,
        "exit_code": 0,
        "rps": 0.0,
        "transfer_kb_s": 0.0,
        "transfer_mb_s": 0.0,
        "lat_avg_ms": 0.0,
        "lat_stdev_ms": 0.0,
        "lat_max_ms": 0.0,
        "p50_ms": 0.0,
        "p75_ms": 0.0,
        "p90_ms": 0.0,
        "p95_ms": 0.0,
        "p99_ms": 0.0,
        "total_requests": 0,
        "read_mb": 0.0,
        "errors_non2xx": 0,
        "socket_connect_errors": 0,
        "socket_read_errors": 0,
        "socket_write_errors": 0,
        "socket_timeout_errors": 0,
        "pi_cpu_busy_avg_pct": 0.0,
        "pi_cpu_busy_max_pct": 0.0,
        "pi_cpu_busy_min_pct": 0.0,
        "pi_cpu_samples": 0,
    }


def main():
    parser = argparse.ArgumentParser(description="Constant-throughput rate sweep using wrk2")
    parser.add_argument("--mode", choices=["vanilla", "proto"], required=True, help="Gateway mode")
    parser.add_argument("--rates", default="50,100,150,200,250,300,350,400,450,500,550,600,650,700", help="Comma-separated rates")
    parser.add_argument("--target-mode", choices=["alternate", "same"], default="alternate", help="Request routing mode: alternate between fns, or send all to the same fn")
    parser.add_argument("--concurrency", type=int, default=100, help="Fixed concurrency level")
    parser.add_argument("--payload-kb", type=int, default=32, help="Payload size in KB")
    parser.add_argument("--out", required=True, help="Output CSV path")
    parser.add_argument("--pi-ssh", default=DEFAULT_PI_SSH, help="Pi SSH connection string")
    parser.add_argument("--duration-s", type=int, default=20, help="Duration of each wrk2 test step")
    parser.add_argument("--timeout-s", type=int, default=10, help="wrk2 request timeout")
    parser.add_argument("--threads", type=int, default=4, help="wrk2 client threads")
    parser.add_argument("--pause", type=int, default=5, help="Pause between rate steps")

    args = parser.parse_args()

    rates = _parse_int_list(args.rates)
    wrk2_bin = _find_wrk2()

    script_dir = Path(__file__).resolve().parent
    lua_script = str(script_dir / "client" / "post_payload.lua")

    if args.mode == "vanilla":
        url = DEFAULT_VANILLA_URL
        fn_a, fn_b = "vanilla-fn-a", "vanilla-fn-b"
    else:
        url = DEFAULT_PROTO_URL
        fn_a, fn_b = "sumprod-timing-fn-a", "sumprod-timing-fn-b"

    print(f"=== wrk2 Constant-Throughput Sweep ({args.mode.upper()}) ===")
    print(f"Target URL: {url}")
    print(f"Rates      : {rates}")
    print(f"Target Mode: {args.target_mode}")
    print(f"Concurrency: {args.concurrency}")
    print(f"Payload KB : {args.payload_kb} KB")
    print(f"wrk2 Bin   : {wrk2_bin}")
    print(f"Out CSV    : {args.out}")
    print()

    rows = []
    for idx, rate in enumerate(rates, start=1):
        print(f"[{idx}/{len(rates)}] Sweeping rate={rate} (C={args.concurrency}, P={args.payload_kb}KB)...")
        
        # Start Pi CPU sampling
        cpu_samples = max(3, args.duration_s + 2)
        cpu_proc = _start_pi_cpu_sampling(args.pi_ssh, cpu_samples, 1)

        # Run wrk2 load
        exit_code, output = _run_wrk2(
            wrk2_bin=wrk2_bin,
            lua_script=lua_script,
            url=url,
            duration_s=args.duration_s,
            timeout_s=args.timeout_s,
            threads=args.threads,
            concurrency=args.concurrency,
            rate=rate,
            payload_kb=args.payload_kb,
            target_mode=args.target_mode,
            fn_a=fn_a,
            fn_b=fn_b
        )

        # Retrieve CPU sampling
        try:
            cpu_stdout, cpu_stderr = cpu_proc.communicate(timeout=args.duration_s + args.timeout_s + 30)
        except subprocess.TimeoutExpired:
            cpu_proc.kill()
            cpu_stdout, cpu_stderr = cpu_proc.communicate()

        cpu_avg, cpu_max, cpu_min, cpu_count = _compute_cpu_busy_stats(cpu_stdout)
        parsed = _parse_wrk2_output(output)

        row = _base_row()
        row.update(parsed)
        row["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
        row["monitored_value"] = rate
        row["rate"] = rate
        row["concurrency"] = args.concurrency
        row["target_mode"] = args.target_mode
        row["payload_kb"] = args.payload_kb
        row["exit_code"] = exit_code
        row["pi_cpu_busy_avg_pct"] = cpu_avg
        row["pi_cpu_busy_max_pct"] = cpu_max
        row["pi_cpu_busy_min_pct"] = cpu_min
        row["pi_cpu_samples"] = cpu_count

        rows.append(row)

        print(
            f"  RPS={row['rps']:7.2f} | Throughput={row['transfer_kb_s']:8.2f} KB/s\n"
            f"  AvgLat={row['lat_avg_ms']:7.2f} ms | p99={row['p99_ms']:7.2f} ms\n"
            f"  Pi CPU Avg={row['pi_cpu_busy_avg_pct']:6.2f}% | Max={row['pi_cpu_busy_max_pct']:6.2f}%\n"
            f"  Timeouts={row['socket_timeout_errors']} | Errors={row['errors_non2xx']}\n"
        )

        if idx < len(rates) and args.pause > 0:
            time.sleep(args.pause)

    # Save to CSV
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(rows[0].keys())
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\n[ok] Sweeps complete. Saved CSV results to: {out_path}")


if __name__ == "__main__":
    main()
