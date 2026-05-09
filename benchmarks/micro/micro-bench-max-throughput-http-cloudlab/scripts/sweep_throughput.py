#!/usr/bin/env python3
"""
sweep_throughput.py — Max-throughput wrk sweep for CloudLab (no CPU pinning).

Usage:
    python3 sweep_throughput.py <payload_kb> <mode: vanilla|proto> <output_csv> [url]

Arguments:
    payload_kb   : POST body size in KB  (e.g. 32)
    mode         : "vanilla" → gateway on :8082
                   "proto"   → gateway on :8083
    output_csv   : path for the result CSV
    url          : optional override URL (default inferred from mode + SERVER_IP env)

The sweep runs 8 fixed (threads, connections) steps:
    step 1 : threads=1, connections=200
    step 2 : threads=2, connections=400
    ...
    step 8 : threads=8, connections=1600

wrk --timeout 10s is used on every step to avoid biasing results with stuck
connections. All 8 steps are always executed (no early stopping).

Output CSV columns:
    wrk_threads, concurrency, rps, transfer_kb_s, transfer_mb_s,
    lat_avg_ms, lat_stdev_ms, lat_max_ms,
    p50_ms, p75_ms, p90_ms, p99_ms,
    total_requests, read_mb, errors_non2xx,
    payload_kb, mode, target_mode,
    resp_kb_per_req, request_upload_mb_s
"""

import re
import subprocess
import csv
import sys
import os
import time

# ---------------------------------------------------------------------------
# Sweep definition: 8 fixed (threads, connections) steps
# ---------------------------------------------------------------------------
SWEEP_STEPS = [
    (1,  200),
    (2,  400),
    (3,  600),
    (4,  800),
    (5, 1000),
    (6, 1200),
    (7, 1400),
    (8, 1600),
]

# wrk duration per step (seconds)
DURATION_S = 20

# wrk per-request timeout — prevents stuck connections from biasing results
WRK_TIMEOUT_S = 10

# Pause between steps (seconds) — let the server drain connections.
# Needs to be long enough for the server to fully close keepalive sockets
# left over from the previous step.  Set INTERACTIVE_STEPS=1 to wait for
# a keyboard Enter instead (useful for manual verification).
PAUSE_BETWEEN_STEPS_S = 15

# Stabilization wait before the first step (seconds)
DEFAULT_STABILIZE_BEFORE_SWEEP_S = 15

# ---------------------------------------------------------------------------
# Default target URLs — override with SERVER_IP env var or pass url on argv
# ---------------------------------------------------------------------------
_DEFAULT_SERVER = os.environ.get("SERVER_IP", "ms0626.utah.cloudlab.us")
DEFAULT_VANILLA_URL = f"http://{_DEFAULT_SERVER}:8082/function/timing-fn-a"
DEFAULT_PROTO_URL   = f"http://{_DEFAULT_SERVER}:8083/function/timing-fn-a"

# ---------------------------------------------------------------------------
# wrk output parser
# ---------------------------------------------------------------------------
_LATENCY_STATS_RE = re.compile(
    r"Latency\s+"
    r"(?P<avg>[0-9]+\.?[0-9]*)\s*(?P<avg_u>us|ms|s)\s+"
    r"(?P<stdev>[0-9]+\.?[0-9]*)\s*(?P<stdev_u>us|ms|s)\s+"
    r"(?P<max>[0-9]+\.?[0-9]*)\s*(?P<max_u>us|ms|s)"
)
_PERCENTILE_RE = re.compile(
    r"(?P<pct>\d+)%\s+(?P<val>[0-9]+\.?[0-9]*)\s*(?P<unit>us|ms|s)"
)
_REQUESTS_IN_RE = re.compile(
    r"(?P<reqs>[0-9,]+)\s+requests in\s+[0-9.]+s,\s+(?P<read>[0-9.]+)(?P<read_u>KB|MB|GB)\s+read"
)
_RPS_RE      = re.compile(r"Requests/sec:\s+(?P<rps>[0-9.]+)")
_TRANSFER_RE = re.compile(r"Transfer/sec:\s+(?P<val>[0-9.]+)(?P<unit>KB|MB|GB)")
_ERRORS_RE   = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")


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


def parse_wrk_output(text: str) -> dict | None:
    """Parse wrk --latency stdout into a metrics dict. Returns None on failure."""
    m = _LATENCY_STATS_RE.search(text)
    if not m:
        return None

    lat_avg_ms   = _to_ms(float(m.group("avg")),   m.group("avg_u"))
    lat_stdev_ms = _to_ms(float(m.group("stdev")), m.group("stdev_u"))
    lat_max_ms   = _to_ms(float(m.group("max")),   m.group("max_u"))

    percentiles: dict[int, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pct = int(pm.group("pct"))
        val = _to_ms(float(pm.group("val")), pm.group("unit"))
        percentiles[pct] = val

    rps_m = _RPS_RE.search(text)
    rps = float(rps_m.group("rps")) if rps_m else 0.0

    tr_m = _TRANSFER_RE.search(text)
    transfer_mbs = _to_mb(float(tr_m.group("val")), tr_m.group("unit")) if tr_m else 0.0

    req_m = _REQUESTS_IN_RE.search(text)
    total_reqs = int(req_m.group("reqs").replace(",", "")) if req_m else 0
    read_mb    = _to_mb(float(req_m.group("read")), req_m.group("read_u")) if req_m else 0.0

    err_m = _ERRORS_RE.search(text)
    errors_non2xx = int(err_m.group("n")) if err_m else 0

    transfer_kbs = round(transfer_mbs * 1024.0, 3)

    return {
        "rps":            round(rps, 2),
        "transfer_kb_s":  transfer_kbs,
        "transfer_mb_s":  round(transfer_mbs, 3),
        "lat_avg_ms":     round(lat_avg_ms, 3),
        "lat_stdev_ms":   round(lat_stdev_ms, 3),
        "lat_max_ms":     round(lat_max_ms, 3),
        "p50_ms":         round(percentiles.get(50, 0.0), 3),
        "p75_ms":         round(percentiles.get(75, 0.0), 3),
        "p90_ms":         round(percentiles.get(90, 0.0), 3),
        "p99_ms":         round(percentiles.get(99, 0.0), 3),
        "total_requests": total_reqs,
        "read_mb":        round(read_mb, 3),
        "errors_non2xx":  errors_non2xx,
    }


# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def run_wrk(threads: int, connections: int, duration_s: int,
            timeout_s: int, url: str, lua_script: str) -> dict | None:
    """Run one wrk step and return parsed metrics, or None on failure."""
    cmd = [
        "wrk",
        f"-t{threads}",
        f"-c{connections}",
        f"-d{duration_s}s",
        "--timeout", f"{timeout_s}s",
        "--latency",
        "-s", lua_script,
        url,
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration_s + timeout_s + 30,
        )
    except subprocess.TimeoutExpired:
        print(f"  [error] wrk process timed out (threads={threads}, connections={connections})")
        return None
    except FileNotFoundError:
        print("  [error] wrk not found — install it: sudo apt install wrk")
        sys.exit(1)

    combined = result.stdout + result.stderr
    parsed = parse_wrk_output(combined)

    if parsed is None:
        print("  [error] could not parse wrk output:")
        print(combined[:500])

    return parsed


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    payload_kb  = int(sys.argv[1])
    mode_arg    = sys.argv[2].lower()
    output_file = sys.argv[3]

    if mode_arg in ("single", "vanilla"):
        mode = "vanilla"
    elif mode_arg == "proto":
        mode = "proto"
    else:
        print(f"[error] Unsupported mode '{mode_arg}'. Use vanilla or proto.")
        sys.exit(1)

    url = sys.argv[4] if len(sys.argv) > 4 else (
        DEFAULT_PROTO_URL if mode == "proto" else DEFAULT_VANILLA_URL
    )

    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_lua = os.path.normpath(
        os.path.join(script_dir, "..", "client", "post_payload.lua")
    )
    lua_script = os.environ.get("WRK_LUA_SCRIPT", default_lua)
    lua_script = os.path.abspath(os.path.expanduser(lua_script))

    if not os.path.exists(lua_script):
        print(f"[error] Lua script not found: {lua_script}")
        sys.exit(1)

    target_mode = os.environ.get("WRK_TARGET_MODE", "same")

    stabilize_before_s = int(
        os.environ.get("STABILIZE_BEFORE_SWEEP_S", str(DEFAULT_STABILIZE_BEFORE_SWEEP_S))
    )

    interactive = os.environ.get("INTERACTIVE_STEPS", "0") == "1"

    print(f"Mode: {mode.upper()} | Payload: {payload_kb}KB")
    print(f"URL: {url}")
    print(f"Lua: {lua_script}")
    print(f"Target mode: {target_mode}")
    print(f"Duration per step: {DURATION_S}s | wrk timeout: {WRK_TIMEOUT_S}s")
    print(f"Sweep steps: {len(SWEEP_STEPS)}  ({SWEEP_STEPS[0]} … {SWEEP_STEPS[-1]})")
    print(f"Interactive pause: {'yes (press Enter between steps)' if interactive else f'no ({PAUSE_BETWEEN_STEPS_S}s auto)'}")
    print()

    if stabilize_before_s > 0:
        print(f"[stabilize] Waiting {stabilize_before_s}s before first step...")
        time.sleep(stabilize_before_s)
        print()

    os.environ["WRK_PAYLOAD_KB"] = str(payload_kb)

    CSV_FIELDS = [
        "wrk_threads", "concurrency",
        "rps", "transfer_kb_s", "transfer_mb_s",
        "lat_avg_ms", "lat_stdev_ms", "lat_max_ms",
        "p50_ms", "p75_ms", "p90_ms", "p99_ms",
        "total_requests", "read_mb", "errors_non2xx",
        "payload_kb", "mode", "target_mode",
        "resp_kb_per_req", "request_upload_mb_s",
    ]

    results = []

    for step_idx, (threads, connections) in enumerate(SWEEP_STEPS, start=1):
        print(f"--- Step {step_idx}/{len(SWEEP_STEPS)}: threads={threads}  connections={connections} ---")
        metrics = run_wrk(threads, connections, DURATION_S, WRK_TIMEOUT_S, url, lua_script)
        if metrics is None:
            print(f"  [skip] no result for threads={threads} connections={connections}")
            time.sleep(PAUSE_BETWEEN_STEPS_S)
            continue

        row = dict(metrics)
        row["wrk_threads"] = threads
        row["concurrency"] = connections
        row["payload_kb"]  = payload_kb
        row["mode"]        = mode
        row["target_mode"] = target_mode
        row["resp_kb_per_req"] = round(
            (row["transfer_kb_s"] / row["rps"]) if row["rps"] > 0 else 0.0,
            3,
        )
        row["request_upload_mb_s"] = round(
            (row["rps"] * payload_kb) / 1024.0,
            3,
        )

        results.append(row)

        print(
            f"  RPS={row['rps']:8.2f}  "
            f"Throughput={row['transfer_kb_s']:8.3f} KB/s  ({row['transfer_mb_s']:.3f} MB/s)  "
            f"Resp/Req={row['resp_kb_per_req']:6.3f} KB  "
            f"ReqUpload={row['request_upload_mb_s']:6.3f} MB/s  "
            f"AvgLat={row['lat_avg_ms']:7.2f}ms  "
            f"p99={row['p99_ms']:7.2f}ms  "
            f"Reqs={row['total_requests']:6d}  "
            f"Errors={row['errors_non2xx']}"
        )

        if step_idx < len(SWEEP_STEPS):
            if interactive:
                input("\n[pause] Press Enter to continue to next step...")
                print()
            else:
                print(f"  [pause] Waiting {PAUSE_BETWEEN_STEPS_S}s for server to drain connections...")
                time.sleep(PAUSE_BETWEEN_STEPS_S)

    if not results:
        print("\n[error] No results collected. Check server and wrk output above.")
        sys.exit(1)

    peak = max(results, key=lambda r: r["transfer_kb_s"])
    print(
        f"\n[peak] threads={peak['wrk_threads']} connections={peak['concurrency']} → "
        f"RPS={peak['rps']:.2f}  "
        f"Throughput={peak['transfer_kb_s']:.3f} KB/s ({peak['transfer_mb_s']:.3f} MB/s)  "
        f"AvgLat={peak['lat_avg_ms']:.2f}ms  p99={peak['p99_ms']:.2f}ms"
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)
    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for row in results:
            writer.writerow({k: row[k] for k in CSV_FIELDS})

    print(f"\n[done] Results written to: {output_file}")


if __name__ == "__main__":
    main()
