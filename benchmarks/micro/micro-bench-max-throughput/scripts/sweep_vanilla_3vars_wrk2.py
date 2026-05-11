#!/usr/bin/env python3
"""
Sweep vanilla HTTP performance on three variables with wrk2:
- request rate
- concurrency
- payload size

Produces one CSV per sweep type and records Pi CPU usage while each test point runs.

Example:
python3 sweep_vanilla_3vars_wrk2.py \
  --rate-values 200,400,800,1200 \
  --concurrency-values 50,100,200 \
  --payload-values-kb 32,64,128,256 \
  --fixed-rate 1200 \
  --fixed-concurrency 200 \
  --fixed-payload-kb 64
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

DEFAULT_URL = "http://192.168.2.2:8080/function/timing-fn-a"
DEFAULT_PI_SSH = "romero@192.168.2.2"
DEFAULT_OUT_DIR = "../results/vanilla_3vars"

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

    # Common percentile aliases
    out["p50_ms"] = round(percentiles.get("p50_ms", 0.0), 3)
    out["p75_ms"] = round(percentiles.get("p75_ms", 0.0), 3)
    out["p90_ms"] = round(percentiles.get("p90_ms", 0.0), 3)
    out["p95_ms"] = round(percentiles.get("p95_ms", 0.0), 3)
    out["p99_ms"] = round(percentiles.get("p99_ms", 0.0), 3)

    return out


def _start_pi_cpu_sampling(pi_ssh: str, samples: int, interval_s: int) -> subprocess.Popen[str]:
    # First line: nproc (number of logical CPUs), then one /proc/stat cpu line per sample.
    # This lets the parser scale to non-aggregated % (4 busy cores → 400%).
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

    # First line is nproc output; default to 1 if missing/unparseable.
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

        # Scale by n_cpus so that 4 fully busy cores → 400%
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
    same_target: str,
    fn_a: str,
    fn_b: str,
) -> Tuple[int, str]:
    env = os.environ.copy()
    env["WRK_PAYLOAD_KB"] = str(payload_kb)
    env["WRK_TARGET_MODE"] = target_mode
    env["WRK_SAME_TARGET"] = same_target
    env["WRK_FN_A"] = fn_a
    env["WRK_FN_B"] = fn_b

    cmd = [
        wrk2_bin,
        f"-t{threads}",
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
        "sweep_type": "",
        "monitored_variable": "",
        "monitored_value": 0,
        "rate": 0,
        "concurrency": 0,
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


def _run_sweep(
    sweep_type: str,
    monitored_var: str,
    values: List[int],
    fixed_rate: int,
    fixed_concurrency: int,
    fixed_payload_kb: int,
    args: argparse.Namespace,
    wrk2_bin: str,
    lua_script: str,
) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []

    print(f"\n=== Sweep: {sweep_type} ({monitored_var}) ===")
    print(f"Values: {values}")

    for idx, monitored_value in enumerate(values, start=1):
        if monitored_var == "rate":
            rate = monitored_value
            concurrency = fixed_concurrency
            payload_kb = fixed_payload_kb
        elif monitored_var == "concurrency":
            rate = fixed_rate
            concurrency = monitored_value
            payload_kb = fixed_payload_kb
        elif monitored_var == "payload_kb":
            rate = fixed_rate
            concurrency = fixed_concurrency
            payload_kb = monitored_value
        else:
            raise ValueError(f"Unsupported monitored variable: {monitored_var}")

        print(
            f"[{idx}/{len(values)}] {monitored_var}={monitored_value} | "
            f"rate={rate} c={concurrency} payload_kb={payload_kb}"
        )

        cpu_samples = max(3, args.duration_s + 2)
        cpu_proc = _start_pi_cpu_sampling(args.pi_ssh, cpu_samples, args.cpu_sample_interval_s)

        exit_code, output = _run_wrk2(
            wrk2_bin=wrk2_bin,
            lua_script=lua_script,
            url=args.url,
            duration_s=args.duration_s,
            timeout_s=args.timeout_s,
            threads=args.wrk_threads,
            concurrency=concurrency,
            rate=rate,
            payload_kb=payload_kb,
            target_mode=args.target_mode,
            same_target=args.same_target,
            fn_a=args.fn_a,
            fn_b=args.fn_b,
        )

        try:
            cpu_stdout, cpu_stderr = cpu_proc.communicate(timeout=args.duration_s + args.timeout_s + 30)
        except subprocess.TimeoutExpired:
            cpu_proc.kill()
            cpu_stdout, cpu_stderr = cpu_proc.communicate()

        if cpu_stderr.strip():
            print(f"  [warn] CPU monitor stderr: {cpu_stderr.strip()[:180]}")

        cpu_avg, cpu_max, cpu_min, cpu_count = _compute_cpu_busy_stats(cpu_stdout)
        parsed = _parse_wrk2_output(output)

        row = _base_row()
        row.update(parsed)
        row["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
        row["sweep_type"] = sweep_type
        row["monitored_variable"] = monitored_var
        row["monitored_value"] = monitored_value
        row["rate"] = rate
        row["concurrency"] = concurrency
        row["payload_kb"] = payload_kb
        row["exit_code"] = exit_code
        row["pi_cpu_busy_avg_pct"] = cpu_avg
        row["pi_cpu_busy_max_pct"] = cpu_max
        row["pi_cpu_busy_min_pct"] = cpu_min
        row["pi_cpu_samples"] = cpu_count

        rows.append(row)

        print(
            f"  exit        : {exit_code}\n"
            f"  rps         : {row['rps']:.3f} req/s\n"
            f"  throughput  : {row['transfer_kb_s']:.3f} KB/s  ({row['transfer_mb_s']:.3f} MB/s)\n"
            f"  lat avg     : {row['lat_avg_ms']:.3f} ms  stdev {row['lat_stdev_ms']:.3f} ms  max {row['lat_max_ms']:.3f} ms\n"
            f"  p50         : {row['p50_ms']:.3f} ms\n"
            f"  p75         : {row['p75_ms']:.3f} ms\n"
            f"  p90         : {row['p90_ms']:.3f} ms\n"
            f"  p95         : {row['p95_ms']:.3f} ms\n"
            f"  p99         : {row['p99_ms']:.3f} ms\n"
            f"  total_reqs  : {row['total_requests']}\n"
            f"  read        : {row['read_mb']:.3f} MB\n"
            f"  errors_non2xx: {row['errors_non2xx']}  sock_connect: {row['socket_connect_errors']}"
            f"  sock_read: {row['socket_read_errors']}  sock_write: {row['socket_write_errors']}"
            f"  sock_timeout: {row['socket_timeout_errors']}\n"
            f"  cpu avg     : {row['pi_cpu_busy_avg_pct']:.3f}%  max: {row['pi_cpu_busy_max_pct']:.3f}%"
            f"  min: {row['pi_cpu_busy_min_pct']:.3f}%  samples: {row['pi_cpu_samples']}"
        )

        if idx < len(values) and args.pause_between_s > 0:
            time.sleep(args.pause_between_s)

    return rows


def _write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def _post_status_code(url: str, insecure_tls: bool = False) -> int:
    cmd = [
        "curl",
        "-s",
        "-o",
        "/dev/null",
        "-w",
        "%{http_code}",
        "--max-time",
        "8",
        "-X",
        "POST",
        "-H",
        "Content-Length: 0",
        url,
    ]
    if insecure_tls:
        cmd.insert(1, "-k")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=12)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return 0
    raw = (result.stdout or "").strip()
    try:
        return int(raw)
    except ValueError:
        return 0


def _preflight_check_endpoints(
    url: str,
    target_mode: str,
    same_target: str,
    fn_a: str,
    fn_b: str,
    preflight_insecure_tls: bool,
) -> None:
    # Input URL should look like .../function/<name>
    marker = "/function/"
    if marker not in url:
        print(f"[warn] Preflight skipped: URL does not contain '{marker}': {url}")
        return

    prefix = url.split(marker, 1)[0] + marker

    checks: List[Tuple[str, str]] = []
    if target_mode == "alternate":
        checks = [(fn_a, prefix + fn_a), (fn_b, prefix + fn_b)]
    else:
        checks = [(same_target, prefix + same_target)]

    failed: List[Tuple[str, str, int]] = []
    for name, endpoint in checks:
        code = _post_status_code(endpoint, insecure_tls=preflight_insecure_tls)
        if code != 200:
            failed.append((name, endpoint, code))

    if failed:
        print("[error] Preflight failed: benchmark function endpoints are not healthy for vanilla run.")
        for name, endpoint, code in failed:
            print(f"  - {name}: POST {endpoint} -> HTTP {code}")
        print("[hint] Deploy vanilla functions before running this script (not proto sendfd workers).")
        sys.exit(2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Vanilla wrk2 3-variable sweeps with Pi CPU capture")

    parser.add_argument("--url", default=DEFAULT_URL, help="Vanilla gateway URL")
    parser.add_argument("--pi-ssh", default=DEFAULT_PI_SSH, help="SSH target for Pi CPU sampling")

    parser.add_argument("--rate-values", default="", help="Comma-separated rate values for rate sweep")
    parser.add_argument("--concurrency-values", default="", help="Comma-separated concurrency values for concurrency sweep")
    parser.add_argument("--payload-values-kb", default="", help="Comma-separated payload sizes in KB for payload sweep")

    parser.add_argument("--fixed-rate", type=int, required=True, help="Fixed rate when not sweeping rate")
    parser.add_argument("--fixed-concurrency", type=int, required=True, help="Fixed concurrency when not sweeping concurrency")
    parser.add_argument("--fixed-payload-kb", type=int, required=True, help="Fixed payload KB when not sweeping payload")

    parser.add_argument("--wrk-threads", type=int, default=4, help="wrk2 client thread count")
    parser.add_argument("--duration-s", type=int, default=20, help="wrk2 duration per point")
    parser.add_argument("--timeout-s", type=int, default=10, help="wrk2 per-request timeout")
    parser.add_argument("--pause-between-s", type=int, default=5, help="Pause between points")
    parser.add_argument("--cpu-sample-interval-s", type=int, default=1, help="Pi CPU sample interval")

    parser.add_argument("--target-mode", choices=["same", "alternate"], default="alternate")
    parser.add_argument("--same-target", default="bench2-fn-a")
    parser.add_argument("--fn-a", default="bench2-fn-a")
    parser.add_argument("--fn-b", default="bench2-fn-b")
    parser.add_argument(
        "--preflight-insecure-tls",
        action="store_true",
        help="Use curl -k for endpoint preflight (needed for self-signed HTTPS endpoints)",
    )

    parser.add_argument(
        "--out-dir",
        default=DEFAULT_OUT_DIR,
        help="Output directory for CSV files",
    )

    args = parser.parse_args()

    if args.fixed_rate <= 0 or args.fixed_concurrency <= 0 or args.fixed_payload_kb <= 0:
        parser.error("Fixed values must be > 0")
    if args.wrk_threads <= 0 or args.duration_s <= 0 or args.timeout_s <= 0:
        parser.error("wrk parameters must be > 0")
    if args.cpu_sample_interval_s <= 0:
        parser.error("cpu-sample-interval-s must be > 0")

    return args


def main() -> None:
    args = parse_args()

    try:
        rate_values = _parse_int_list(args.rate_values)
        concurrency_values = _parse_int_list(args.concurrency_values)
        payload_values = _parse_int_list(args.payload_values_kb)
    except ValueError as exc:
        print(f"[error] {exc}")
        sys.exit(2)

    if not rate_values and not concurrency_values and not payload_values:
        print("[error] Provide at least one values list: --rate-values, --concurrency-values, or --payload-values-kb")
        sys.exit(2)

    script_dir = Path(__file__).resolve().parent
    lua_script = Path(os.environ.get("WRK_LUA_SCRIPT", script_dir.parent / "client" / "post_payload.lua"))
    lua_script = lua_script.expanduser().resolve()
    if not lua_script.exists():
        print(f"[error] Lua script not found: {lua_script}")
        sys.exit(2)

    wrk2_bin = _find_wrk2()

    print("=== Vanilla 3-variable wrk2 sweep ===")
    print(f"URL: {args.url}")
    print(f"Pi SSH: {args.pi_ssh}")
    print(f"wrk2: {wrk2_bin}")
    print(f"Lua: {lua_script}")
    print(f"Fixed values -> rate={args.fixed_rate}, concurrency={args.fixed_concurrency}, payload_kb={args.fixed_payload_kb}")

    _preflight_check_endpoints(
        url=args.url,
        target_mode=args.target_mode,
        same_target=args.same_target,
        fn_a=args.fn_a,
        fn_b=args.fn_b,
        preflight_insecure_tls=args.preflight_insecure_tls,
    )

    out_dir_arg = Path(args.out_dir).expanduser()
    if out_dir_arg.is_absolute():
        out_dir = out_dir_arg.resolve()
    elif args.out_dir == DEFAULT_OUT_DIR:
        # Keep historical behavior for the script default.
        out_dir = (script_dir / out_dir_arg).resolve()
    else:
        # For user-provided relative paths, resolve from current working directory.
        out_dir = (Path.cwd() / out_dir_arg).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if rate_values:
        rows = _run_sweep(
            sweep_type="rate_sweep",
            monitored_var="rate",
            values=rate_values,
            fixed_rate=args.fixed_rate,
            fixed_concurrency=args.fixed_concurrency,
            fixed_payload_kb=args.fixed_payload_kb,
            args=args,
            wrk2_bin=wrk2_bin,
            lua_script=str(lua_script),
        )
        _write_csv(out_dir / "rate_sweep.csv", rows)
        print(f"[done] wrote {(out_dir / 'rate_sweep.csv')}")

    if concurrency_values:
        rows = _run_sweep(
            sweep_type="concurrency_sweep",
            monitored_var="concurrency",
            values=concurrency_values,
            fixed_rate=args.fixed_rate,
            fixed_concurrency=args.fixed_concurrency,
            fixed_payload_kb=args.fixed_payload_kb,
            args=args,
            wrk2_bin=wrk2_bin,
            lua_script=str(lua_script),
        )
        _write_csv(out_dir / "concurrency_sweep.csv", rows)
        print(f"[done] wrote {(out_dir / 'concurrency_sweep.csv')}")

    if payload_values:
        rows = _run_sweep(
            sweep_type="payload_sweep",
            monitored_var="payload_kb",
            values=payload_values,
            fixed_rate=args.fixed_rate,
            fixed_concurrency=args.fixed_concurrency,
            fixed_payload_kb=args.fixed_payload_kb,
            args=args,
            wrk2_bin=wrk2_bin,
            lua_script=str(lua_script),
        )
        _write_csv(out_dir / "payload_sweep.csv", rows)
        print(f"[done] wrote {(out_dir / 'payload_sweep.csv')}")


if __name__ == "__main__":
    main()
