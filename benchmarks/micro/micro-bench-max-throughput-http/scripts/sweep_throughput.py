#!/usr/bin/env python3
"""
sweep_throughput.py — Max-throughput sweep (HTTP variant, Raspberry Pi).

Two invocation modes are supported:

  PINNED MODE (original — CPU affinity must be set by pi_pin_all.sh first):
    python3 sweep_throughput.py <payload_kb> <num_cores> <mode> <output_csv> [url]

  NO-PIN MODE (new — no CPU affinity, CloudLab-style fixed steps, uses wrk2):
    python3 sweep_throughput.py <payload_kb> <mode> <output_csv> [url]

  Auto-detected: if the second argument is a digit it is <num_cores> (pinned
  mode); otherwise it is <mode> (no-pin mode).

Arguments:
    payload_kb   : POST body size in KB  (e.g. 32)
    num_cores    : (pinned mode only) CPU cores the Pi is pinned to (1–4).
                   Only used to label the CSV; pinning is done by pi_pin_all.sh.
    mode         : "vanilla" or "single" → native faasd gateway on :8080
                   "proto"               → gateway on :8083 HTTP
    output_csv   : path for the result CSV
    url          : optional override URL (default inferred from mode)

PINNED MODE  — uses wrk, sweeps concurrency 1,11,21,…,401, stops on plateau.
NO-PIN MODE  — uses wrk2 (--rate 999999 to saturate), fixed 4 steps:
               (1 thread, 200 conn) → (2, 400) → (3, 600) → (4, 800).
               All 4 steps always execute (no early stopping).

Useful environment overrides:
    WRK_DURATION_S                  duration per step for both modes
    WRK_THREADS                     client thread count in pinned mode
    PAUSE_BETWEEN_STEPS_S           pause between steps in pinned mode
    NOPIN_STEPS                     no-pin steps, e.g. "1:200,2:400" or "1x200"
    WRK2_RATE                       wrk2 target rate in no-pin mode
    WRK2_TIMEOUT_S                  wrk2 timeout (seconds) in no-pin mode
    PAUSE_BETWEEN_NOPIN_STEPS_S     pause between no-pin steps

wrk / wrk2 must be installed.  Lua defaults to client/post_payload.lua.
Override with WRK_LUA_SCRIPT env var.

Output CSV columns (pinned mode):
    concurrency, rps, transfer_kb_s, transfer_mb_s, lat_avg_ms, lat_stdev_ms,
    lat_max_ms, p50_ms, p75_ms, p90_ms, p99_ms, total_requests, read_mb,
    errors_non2xx, timeouts, payload_kb, num_cores, mode, target_mode,
    resp_kb_per_req, request_upload_mb_s

Output CSV columns (no-pin mode):
    wrk_threads, concurrency, rps, transfer_kb_s, transfer_mb_s,
    lat_avg_ms, lat_stdev_ms, lat_max_ms, p50_ms, p75_ms, p90_ms, p99_ms,
    total_requests, read_mb, errors_non2xx, timeouts, payload_kb, mode,
    target_mode, resp_kb_per_req, request_upload_mb_s
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
DEFAULT_VANILLA_URL = "http://192.168.2.2:8080/function/timing-fn-a"
DEFAULT_PROTO_URL   = "http://192.168.2.2:8083/function/timing-fn-a"

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
DEFAULT_PLATEAU_PATIENCE = 5

# Stop immediately after this many consecutive zero-RPS steps (server dead/saturated).
MAX_CONSECUTIVE_ZERO_RPS = 2

# Pause before each sweep invocation (typically after re-pinning to a new core
# mask). Can be overridden with STABILIZE_BEFORE_SWEEP_S env var.
DEFAULT_STABILIZE_BEFORE_SWEEP_S = 20

# ---------------------------------------------------------------------------
# No-pin mode: fixed (threads, connections) sweep steps (CloudLab-style)
# ---------------------------------------------------------------------------
SWEEP_STEPS_NOPIN = [
    (1, 200),
    (2, 400),
    (3, 600),
    (4, 800),
]

# wrk2 target rate for no-pin mode — set very high to saturate the server.
WRK2_RATE = 999999

# wrk2 per-request timeout (seconds).
WRK2_TIMEOUT_S = 10

# Pause between no-pin steps (seconds) — let the server drain connections.
PAUSE_BETWEEN_NOPIN_STEPS_S = 15

# Stabilization wait before the no-pin sweep (seconds).
DEFAULT_STABILIZE_BEFORE_NOPIN_S = 15

# ---------------------------------------------------------------------------
# wrk / wrk2 output parser
# ---------------------------------------------------------------------------
_LATENCY_STATS_RE = re.compile(
    r"Latency\s+"
    r"(?P<avg>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<avg_u>us|ms|s)\s+"
    r"(?P<stdev>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<stdev_u>us|ms|s)\s+"
    r"(?P<max>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<max_u>us|ms|s)"
)
# Handles both wrk ("50%") and wrk2 ("50.000%") percentage formats.
_PERCENTILE_RE = re.compile(
    r"(?P<pct>[0-9]+(?:\.[0-9]+)?)%\s+(?P<val>[0-9]+\.?[0-9]*)\s*(?P<unit>us|ms|s)"
)
_REQUESTS_IN_RE = re.compile(
    r"(?P<reqs>[0-9,]+)\s+requests in\s+[0-9.]+s,\s+(?P<read>[0-9.]+)(?P<read_u>KB|MB|GB)\s+read"
)
_RPS_RE      = re.compile(r"Requests/sec:\s+(?P<rps>[0-9.]+)")
_TRANSFER_RE = re.compile(r"Transfer/sec:\s+(?P<val>[0-9.]+)(?P<unit>KB|MB|GB)")
_ERRORS_RE        = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")
_SOCKET_ERRORS_RE = re.compile(r"Socket errors:.*?timeout\s+(?P<n>\d+)")


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


def parse_wrk_output(text: str) -> dict | None:
    """Parse wrk/wrk2 stdout into a metrics dict. Returns None only if no metrics are present."""
    m = _LATENCY_STATS_RE.search(text)

    lat_avg_ms = 0.0
    lat_stdev_ms = 0.0
    lat_max_ms = 0.0
    if m:
        lat_avg_ms = _to_ms(_parse_float_or_zero(m.group("avg")), m.group("avg_u"))
        lat_stdev_ms = _to_ms(_parse_float_or_zero(m.group("stdev")), m.group("stdev_u"))
        lat_max_ms = _to_ms(_parse_float_or_zero(m.group("max")), m.group("max_u"))

    percentiles: dict[int, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pct = round(float(pm.group("pct")))  # handles wrk "50" and wrk2 "50.000"
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

    err_sock_m = _SOCKET_ERRORS_RE.search(text)
    timeouts = int(err_sock_m.group("n")) if err_sock_m else 0

    if not any((m, rps_m, tr_m, req_m, err_m, err_sock_m)):
        return None

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
        "timeouts":       timeouts,
    }


def _env_int(name: str, default: int) -> int:
    """Read integer env var with fallback and clear warnings on bad values."""
    raw = os.environ.get(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError:
        print(f"[warn] Invalid integer for {name}={raw!r}; using default {default}")
        return default


def _parse_nopin_steps(raw: str) -> list[tuple[int, int]]:
    """
    Parse NOPIN_STEPS env var into [(threads, connections), ...].
    Accepted forms per item: "T:C", "TxC", or "T,C".
    Example: "1:200,2:400,3:600,4:800"
    """
    out: list[tuple[int, int]] = []
    for item in raw.split(","):
        part = item.strip()
        if not part:
            continue
        m = re.match(r"^(\d+)\s*[:x,]\s*(\d+)$", part)
        if not m:
            raise ValueError(
                f"invalid step '{part}' (expected T:C, TxC, or T,C)"
            )
        t = int(m.group(1))
        c = int(m.group(2))
        if t <= 0 or c <= 0:
            raise ValueError(f"invalid step '{part}' (values must be > 0)")
        out.append((t, c))

    if not out:
        raise ValueError("NOPIN_STEPS produced an empty list")
    return out


# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def run_wrk(concurrency: int, duration_s: int, threads: int, url: str,
            lua_script: str) -> dict | None:
    """Run one wrk step and return parsed metrics, or None on failure."""
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


def _find_wrk2() -> str:
    """Return the wrk2 binary path, checking WRK2 env var and common locations."""
    # 1. Explicit override via environment variable
    env_path = os.environ.get("WRK2")
    if env_path:
        return os.path.expanduser(env_path)
    # 2. wrk2 repo compiles the binary as 'wrk' inside ~/wrk2/
    candidate = os.path.expanduser("~/wrk2/wrk")
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate
    # 3. Standalone wrk2 binary on PATH
    candidate2 = os.path.expanduser("~/wrk2/wrk2")
    if os.path.isfile(candidate2) and os.access(candidate2, os.X_OK):
        return candidate2
    # 4. Assume it's on PATH as 'wrk2'
    return "wrk2"


def run_wrk2(threads: int, connections: int, duration_s: int,
             timeout_s: int, rate: int, url: str, lua_script: str) -> dict | None:
    """Run one wrk2 step at the given target rate and return parsed metrics."""
    wrk2_bin = _find_wrk2()
    cmd = [
        wrk2_bin,
        f"-t{threads}",
        f"-c{connections}",
        f"-d{duration_s}s",
        f"-R{rate}",
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
        print(f"  [error] wrk2 process timed out (threads={threads}, connections={connections})")
        return None
    except FileNotFoundError:
        print(f"  [error] wrk2 not found at '{wrk2_bin}'")
        print("  [hint]  set WRK2=/path/to/wrk2 env var, or build it: cd ~/wrk2 && make")
        sys.exit(1)

    combined = result.stdout + result.stderr
    parsed = parse_wrk_output(combined)

    if parsed is None:
        print("  [error] could not parse wrk2 output:")
        print(combined[:500])

    return parsed


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    # Auto-detect invocation mode: if argv[2] is a digit → pinned mode, else no-pin.
    if sys.argv[2].isdigit():
        if len(sys.argv) < 5:
            print(__doc__)
            sys.exit(1)
        payload_kb   = int(sys.argv[1])
        num_cores    = int(sys.argv[2])
        mode_arg     = sys.argv[3].lower()
        output_file  = sys.argv[4]
        _nopin       = False
        _url_idx     = 5
    else:
        payload_kb   = int(sys.argv[1])
        num_cores    = None
        mode_arg     = sys.argv[2].lower()
        output_file  = sys.argv[3]
        _nopin       = True
        _url_idx     = 4

    # (kept for compatibility with the old 4-arg form)
    _ = payload_kb

    if mode_arg in ("single", "vanilla"):
        mode = "vanilla"
    elif mode_arg == "proto":
        mode = "proto"
    else:
        print(f"[error] Unsupported mode '{mode_arg}'. Use vanilla, single, or proto.")
        sys.exit(1)

    url = sys.argv[_url_idx] if len(sys.argv) > _url_idx else (
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
    os.environ["WRK_PAYLOAD_KB"] = str(payload_kb)

    duration_s = _env_int("WRK_DURATION_S", DURATION_S)
    pinned_wrk_threads = _env_int("WRK_THREADS", WRK_THREADS)
    pause_between_steps_s = _env_int("PAUSE_BETWEEN_STEPS_S", PAUSE_BETWEEN_STEPS_S)
    wrk2_rate = _env_int("WRK2_RATE", WRK2_RATE)
    wrk2_timeout_s = _env_int("WRK2_TIMEOUT_S", WRK2_TIMEOUT_S)
    pause_between_nopin_steps_s = _env_int(
        "PAUSE_BETWEEN_NOPIN_STEPS_S", PAUSE_BETWEEN_NOPIN_STEPS_S
    )

    nopin_steps_env = os.environ.get("NOPIN_STEPS")
    if nopin_steps_env:
        try:
            nopin_steps = _parse_nopin_steps(nopin_steps_env)
        except ValueError as exc:
            print(f"[error] Bad NOPIN_STEPS: {exc}")
            sys.exit(1)
    else:
        nopin_steps = list(SWEEP_STEPS_NOPIN)

    if _nopin:
        # =======================================================================
        # NO-PIN MODE: CloudLab-style fixed (threads, connections) steps, wrk2
        # =======================================================================
        stabilize_before_s = int(
            os.environ.get("STABILIZE_BEFORE_SWEEP_S", str(DEFAULT_STABILIZE_BEFORE_NOPIN_S))
        )
        print(f"Mode: {mode.upper()} | Payload: {payload_kb}KB | [NO-PIN / wrk2]")
        print(f"URL: {url}")
        print(f"Lua: {lua_script}")
        print(f"Target mode: {target_mode}")
        print(f"Duration per step: {duration_s}s | wrk2 rate: {wrk2_rate} | timeout: {wrk2_timeout_s}s")
        print(f"Sweep steps: {nopin_steps}")
        print(f"Pause between steps: {pause_between_nopin_steps_s}s")
        print()

        if stabilize_before_s > 0:
            print(f"[stabilize] Waiting {stabilize_before_s}s before first step...")
            time.sleep(stabilize_before_s)
            print()

        CSV_FIELDS = [
            "wrk_threads", "concurrency",
            "rps", "transfer_kb_s", "transfer_mb_s",
            "lat_avg_ms", "lat_stdev_ms", "lat_max_ms",
            "p50_ms", "p75_ms", "p90_ms", "p99_ms",
            "total_requests", "read_mb", "errors_non2xx", "timeouts",
            "payload_kb", "mode", "target_mode",
            "resp_kb_per_req", "request_upload_mb_s",
        ]

        results = []

        for step_idx, (threads, connections) in enumerate(nopin_steps, start=1):
            print(f"--- Step {step_idx}/{len(nopin_steps)}: threads={threads}  connections={connections} ---")
            metrics = run_wrk2(threads, connections, duration_s, wrk2_timeout_s,
                               wrk2_rate, url, lua_script)
            if metrics is None:
                print(f"  [skip] no result for threads={threads} connections={connections}")
                time.sleep(pause_between_nopin_steps_s)
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
                f"Throughput={row['transfer_kb_s']:8.3f} KB/s ({row['transfer_mb_s']:.3f} MB/s)  "
                f"Resp/Req={row['resp_kb_per_req']:6.3f} KB  "
                f"ReqUpload={row['request_upload_mb_s']:6.3f} MB/s  "
                f"AvgLat={row['lat_avg_ms']:7.2f}ms  "
                f"p99={row['p99_ms']:7.2f}ms  "
                f"Reqs={row['total_requests']:6d}  "
                f"Errors={row['errors_non2xx']}  "
                f"Timeouts={row['timeouts']}"
            )

            if step_idx < len(nopin_steps):
                print(f"  [pause] Waiting {pause_between_nopin_steps_s}s for server to drain connections...")
                time.sleep(pause_between_nopin_steps_s)

        if not results:
            print("\n[error] No results collected. Check server and wrk2 output above.")
            sys.exit(1)

        peak = max(results, key=lambda r: r["transfer_kb_s"])
        print(
            f"\n[peak] threads={peak['wrk_threads']} connections={peak['concurrency']} → "
            f"RPS={peak['rps']:.2f}  "
            f"Throughput={peak['transfer_kb_s']:.3f} KB/s ({peak['transfer_mb_s']:.3f} MB/s)  "
            f"AvgLat={peak['lat_avg_ms']:.2f}ms  p99={peak['p99_ms']:.2f}ms  "
            f"Timeouts={peak['timeouts']}"
        )

    else:
        # =======================================================================
        # PINNED MODE: original variable-concurrency sweep with wrk
        # =======================================================================
        stabilize_before_s = int(
            os.environ.get("STABILIZE_BEFORE_SWEEP_S", str(DEFAULT_STABILIZE_BEFORE_SWEEP_S))
        )
        print(f"Mode: {mode.upper()} | Cores: {num_cores} | Payload: {payload_kb}KB | [PINNED / wrk]")
        print(f"URL: {url}")
        print(f"Lua: {lua_script}")
        print(f"Target mode: {target_mode}")
        print(f"Duration per step: {duration_s}s | wrk threads: {pinned_wrk_threads}")
        print()

        if stabilize_before_s > 0:
            print(f"[stabilize] Waiting {stabilize_before_s}s before sweep...")
            time.sleep(stabilize_before_s)
            print()

        CSV_FIELDS = [
            "concurrency", "rps", "transfer_kb_s", "transfer_mb_s",
            "lat_avg_ms", "lat_stdev_ms", "lat_max_ms",
            "p50_ms", "p75_ms", "p90_ms", "p99_ms",
            "total_requests", "read_mb", "errors_non2xx", "timeouts",
            "payload_kb", "num_cores", "mode", "target_mode",
            "resp_kb_per_req", "request_upload_mb_s",
        ]

        results = []

        plateau_patience = int(
            os.environ.get("PLATEAU_PATIENCE", str(DEFAULT_PLATEAU_PATIENCE))
        )
        best_kb_s = 0.0
        steps_without_improvement = 0
        consecutive_zeros = 0

        for c in CONCURRENCIES:
            print(f"--- C={c} concurrent connections ---")
            metrics = run_wrk(c, duration_s, pinned_wrk_threads, url, lua_script)
            if metrics is None:
                print(f"  [skip] no result for C={c}")
                time.sleep(pause_between_steps_s)
                continue

            row = dict(metrics)
            row["concurrency"]  = c
            row["payload_kb"]   = payload_kb
            row["num_cores"]    = num_cores
            row["mode"]         = mode
            row["target_mode"]  = target_mode
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
                f"Errors={row['errors_non2xx']}  "
                f"Timeouts={row['timeouts']}"
            )

            # Zero-RPS guard: stop fast when server is fully saturated
            if row["rps"] == 0.0:
                consecutive_zeros += 1
                if consecutive_zeros >= MAX_CONSECUTIVE_ZERO_RPS:
                    print(
                        f"\n[early-stop] {consecutive_zeros} consecutive zero-RPS steps "
                        f"— server is saturated. Stopping sweep."
                    )
                    break
            else:
                consecutive_zeros = 0

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

            time.sleep(pause_between_steps_s)

        if not results:
            print("\n[error] No results collected. Check server and wrk output above.")
            sys.exit(1)

        peak = max(results, key=lambda r: r["transfer_kb_s"])
        print(
            f"\n[peak] C={peak['concurrency']} → "
            f"RPS={peak['rps']:.2f}  "
            f"Throughput={peak['transfer_kb_s']:.3f} KB/s ({peak['transfer_mb_s']:.3f} MB/s)  "
            f"AvgLat={peak['lat_avg_ms']:.2f}ms  p99={peak['p99_ms']:.2f}ms  "
            f"Timeouts={peak['timeouts']}"
        )

    # =========================================================================
    # Write CSV (common to both modes)
    # =========================================================================
    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)
    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for row in results:
            writer.writerow({k: row[k] for k in CSV_FIELDS})

    print(f"\n[done] Results written to: {output_file}")


if __name__ == "__main__":
    main()
