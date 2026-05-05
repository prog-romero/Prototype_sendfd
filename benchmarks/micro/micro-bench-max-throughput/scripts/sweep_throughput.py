#!/usr/bin/env python3
"""
sweep_throughput.py — Max-throughput concurrency sweep using wrk.

Usage:
    python3 sweep_throughput.py <payload_kb> <num_cores> <mode: vanilla|single|proto> <output_csv> [url]

Arguments:
    payload_kb   : POST body size in KB  (e.g. 32)
    num_cores    : CPU cores the Pi server is pinned to (1, 2, 3, or 4).
                   Only used to label the CSV; pinning itself is done by
                   pi_pin_all.sh before running this script.
    mode         : "vanilla" or "single" → vanilla gateway path (:8444)
                   "proto"               → prototype gateway path (:9444)
    output_csv   : path for the result CSV
    url          : optional override URL (default inferred from mode)

wrk must be installed.  By default the Lua script is
client/post_payload.lua next to this script's parent directory.
You can override it with WRK_LUA_SCRIPT=/absolute/or/relative/path.lua.

Output CSV columns:
    concurrency, rps, transfer_mb_s, lat_avg_ms, lat_stdev_ms, lat_max_ms,
    p50_ms, p75_ms, p90_ms, p99_ms, total_requests, read_mb, errors_non2xx,
    payload_kb, num_cores, mode, target_mode, resp_kb_per_req,
    request_upload_mb_s
"""

import re
import subprocess
import csv
import sys
import os
import time

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DEFAULT_VANILLA_URL = "https://192.168.2.2:8444/function/bench2-fn-a"
DEFAULT_PROTO_URL   = "https://192.168.2.2:9444/function/bench2-fn-a"

# Concurrency levels to sweep: 1, 11, 21, 31, ... 401 (step of 10).
CONCURRENCIES = list(range(1, 402, 10))

# wrk duration per step (seconds).  20 s gives a stable average.
DURATION_S = 20

# wrk threads on the client machine.
WRK_THREADS = 4

# Pause between steps so the Pi can drain connections.
PAUSE_BETWEEN_STEPS_S = 3

# Number of consecutive non-improving throughput points before stopping.
# Can be overridden with PLATEAU_PATIENCE env var.
DEFAULT_PLATEAU_PATIENCE = 20

# Pause before each sweep invocation (typically after re-pinning to a new core
# mask). Can be overridden with STABILIZE_BEFORE_SWEEP_S env var.
DEFAULT_STABILIZE_BEFORE_SWEEP_S = 20

# ---------------------------------------------------------------------------
# wrk output parser
# ---------------------------------------------------------------------------
# Example wrk --latency output:
#   Running 20s test @ https://...
#     4 threads and 16 connections
#     Thread Stats   Avg      Stdev     Max   +/- Stdev
#       Latency   123.45ms   45.67ms   1.23s    80.00%
#       Req/Sec    32.10     12.34    120.00    75.00%
#     Latency Distribution
#          50%   98.76ms
#          75%  145.23ms
#          90%  234.56ms
#          99%  987.65ms
#     640 requests in 20.00s, 20.00MB read
#   Requests/sec:     32.00
#   Transfer/sec:      1.00MB

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
    """Convert a wrk time value to milliseconds."""
    if unit == "us":
        return value / 1000.0
    if unit == "ms":
        return value
    if unit == "s":
        return value * 1000.0
    return value


def _to_mb(value: float, unit: str) -> float:
    """Convert a wrk data value to MB."""
    if unit == "KB":
        return value / 1024.0
    if unit == "MB":
        return value
    if unit == "GB":
        return value * 1024.0
    return value


def parse_wrk_output(text: str) -> dict | None:
    """Parse wrk --latency stdout into a metrics dict.  Returns None on failure."""
    m = _LATENCY_STATS_RE.search(text)
    if not m:
        return None

    lat_avg_ms   = _to_ms(float(m.group("avg")),   m.group("avg_u"))
    lat_stdev_ms = _to_ms(float(m.group("stdev")), m.group("stdev_u"))
    lat_max_ms   = _to_ms(float(m.group("max")),   m.group("max_u"))

    percentiles: dict[int, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pct  = int(pm.group("pct"))
        val  = _to_ms(float(pm.group("val")), pm.group("unit"))
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
        "rps":           round(rps, 2),
        "transfer_kb_s": transfer_kbs,
        "transfer_mb_s": round(transfer_mbs, 3),
        "lat_avg_ms":    round(lat_avg_ms, 3),
        "lat_stdev_ms":  round(lat_stdev_ms, 3),
        "lat_max_ms":    round(lat_max_ms, 3),
        "p50_ms":        round(percentiles.get(50, 0.0), 3),
        "p75_ms":        round(percentiles.get(75, 0.0), 3),
        "p90_ms":        round(percentiles.get(90, 0.0), 3),
        "p99_ms":        round(percentiles.get(99, 0.0), 3),
        "total_requests":  total_reqs,
        "read_mb":         round(read_mb, 3),
        "errors_non2xx":   errors_non2xx,
    }


# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def run_wrk(concurrency: int, duration_s: int, threads: int, url: str,
            lua_script: str) -> dict | None:
    """Run one wrk step and return parsed metrics, or None on failure."""
    # wrk requires connections >= threads
    actual_threads = min(threads, concurrency)
    cmd = [
        "wrk",
        f"-t{actual_threads}",
        f"-c{concurrency}",
        f"-d{duration_s}s",
        "--latency",
        "-s", lua_script,
        url,
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration_s + 60,
        )
    except subprocess.TimeoutExpired:
        print("  [error] wrk timed out")
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
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)

    payload_kb  = int(sys.argv[1])
    num_cores   = int(sys.argv[2])
    mode_arg    = sys.argv[3].lower()
    output_file = sys.argv[4]
    if mode_arg in ("single", "vanilla"):
        mode = "vanilla"
    elif mode_arg == "proto":
        mode = "proto"
    else:
        print(f"[error] Unsupported mode '{mode_arg}'. Use vanilla, single, or proto.")
        sys.exit(1)

    url = sys.argv[5] if len(sys.argv) > 5 else (
        DEFAULT_PROTO_URL if mode == "proto" else DEFAULT_VANILLA_URL
    )

    # Lua script lives in client/ by default; allow env override for variants.
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

    print(f"Mode: {mode.upper()} | Cores: {num_cores} | Payload: {payload_kb}KB")
    print(f"URL: {url}")
    print(f"Lua: {lua_script}")
    print(f"Target mode: {target_mode}")
    print(f"Duration per step: {DURATION_S}s | wrk threads: {WRK_THREADS}")
    print()

    stabilize_before_s = int(
        os.environ.get("STABILIZE_BEFORE_SWEEP_S", str(DEFAULT_STABILIZE_BEFORE_SWEEP_S))
    )
    if stabilize_before_s > 0:
        print(f"Stabilization wait before sweep: {stabilize_before_s}s")
        time.sleep(stabilize_before_s)
        print()

    os.environ["WRK_PAYLOAD_KB"] = str(payload_kb)

    CSV_FIELDS = [
        "concurrency", "rps", "transfer_kb_s", "transfer_mb_s",
        "lat_avg_ms", "lat_stdev_ms", "lat_max_ms",
        "p50_ms", "p75_ms", "p90_ms", "p99_ms",
        "total_requests", "read_mb", "errors_non2xx",
        "payload_kb", "num_cores", "mode", "target_mode",
        "resp_kb_per_req", "request_upload_mb_s",
    ]

    results = []

    # Early-stop: if throughput does not improve for this many consecutive
    # steps, the server has saturated — no point going higher.
    plateau_patience = int(
        os.environ.get("PLATEAU_PATIENCE", str(DEFAULT_PLATEAU_PATIENCE))
    )
    best_kb_s = 0.0
    steps_without_improvement = 0

    for c in CONCURRENCIES:
        print(f"--- C={c} concurrent connections ---")
        metrics = run_wrk(c, DURATION_S, WRK_THREADS, url, lua_script)
        if metrics is None:
            print(f"  [skip] no result for C={c}")
            time.sleep(PAUSE_BETWEEN_STEPS_S)
            continue

        row = dict(metrics)
        row["concurrency"] = c
        row["payload_kb"]  = payload_kb
        row["num_cores"]   = num_cores
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

        if row["transfer_kb_s"] > best_kb_s:
            best_kb_s = row["transfer_kb_s"]
            steps_without_improvement = 0
        else:
            steps_without_improvement += 1
            if steps_without_improvement >= plateau_patience:
                print(
                    f"\n[early-stop] Throughput did not improve for "
                    f"{plateau_patience} consecutive steps "
                    f"(best={best_kb_s:.3f} KB/s). Stopping sweep."
                )
                break

        time.sleep(PAUSE_BETWEEN_STEPS_S)

    if not results:
        print("\n[error] No results collected. Check server and wrk output above.")
        sys.exit(1)

    # Identify where throughput saturates
    peak = max(results, key=lambda r: r["transfer_kb_s"])
    print(f"\nPeak throughput: {peak['transfer_kb_s']} KB/s ({peak['transfer_mb_s']} MB/s) at C={peak['concurrency']} "
          f"({peak['rps']} req/s, p99={peak['p99_ms']}ms)")

    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)

    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(results)

    print(f"\nDone! Results saved to {output_file}")


if __name__ == "__main__":
    main()

