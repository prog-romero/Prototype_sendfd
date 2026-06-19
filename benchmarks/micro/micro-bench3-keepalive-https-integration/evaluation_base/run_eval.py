#!/usr/bin/env python3
"""
Baseline latency evaluation — one NEW connection per request, no keepalive.

Uses wrk2 with -c 1 (one connection slot) and a Lua script that adds
"Connection: close" to every request.  wrk2 therefore opens a brand-new
TCP+TLS connection for each request, so the reported latency is:

  TCP SYN/ACK + TLS handshake + HTTP request/response round-trip

What is measured
----------------
Prototype  (--mode proto):
    client → gateway (TLS peek + sendfd) → provider.sock → watchdog sock
    → function → function replies DIRECTLY to client
    (no return hop through faasd components)

Vanilla  (--mode vanilla):
    client → gateway → faasd-provider → watchdog → function
    → watchdog → faasd-provider → gateway → client
    (standard OpenFaaS HTTP proxy chain)

For each payload size, wrk2 sends approximately --requests requests at --rate
req/s (-c 1 -t 1), collects the latency distribution and saves the stats
(avg, stdev, p50/75/90/99) to a CSV row.

Usage
-----
  # Prototype:
  python3 run_eval.py --mode proto --host 192.168.2.2 \\
      --sizes 1,5,10,20,30,40,50 --requests 50 \\
      --output results/proto.csv

  # Vanilla:
  python3 run_eval.py --mode vanilla --host 192.168.2.2 \\
      --sizes 1,5,10,20,30,40,50 --requests 50 \\
      --output results/vanilla.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

# ── wrk2 output parsers (identical to sweep_throughput_wrk2.py) ──────────────

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
_REQUESTS_IN_RE = re.compile(r"(?P<reqs>[0-9,]+)\s+requests in")
_ERRORS_RE = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")
_SOCKET_ERRORS_RE = re.compile(
    r"Socket errors:\s*connect\s+(?P<connect>[0-9]+),\s*read\s+(?P<read>[0-9]+),"
    r"\s*write\s+(?P<write>[0-9]+),\s*timeout\s+(?P<timeout>[0-9]+)"
)


def _to_ms(value: float, unit: str) -> float:
    if unit == "us":
        return value / 1000.0
    if unit == "ms":
        return value
    if unit == "s":
        return value * 1000.0
    return value


def _parse_float_or_zero(raw: str) -> float:
    low = raw.strip().lower()
    if low in {"nan", "-nan", "+nan"}:
        return 0.0
    return float(raw)


def _parse_wrk2_output(text: str) -> dict:
    m = _LATENCY_STATS_RE.search(text)
    if m:
        avg_ms   = _to_ms(_parse_float_or_zero(m.group("avg")),   m.group("avg_u"))
        stdev_ms = _to_ms(_parse_float_or_zero(m.group("stdev")), m.group("stdev_u"))
        max_ms   = _to_ms(_parse_float_or_zero(m.group("max")),   m.group("max_u"))
    else:
        avg_ms = stdev_ms = max_ms = 0.0
        print("  [WARN] wrk2 Latency line not found — raw output:")
        for line in text.splitlines():
            print(f"    | {line}")

    pcts: dict[str, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        key = f"p{float(pm.group('pct')):g}_ms"
        pcts[key] = _to_ms(float(pm.group("val")), pm.group("unit"))

    req_m  = _REQUESTS_IN_RE.search(text)
    err_m  = _ERRORS_RE.search(text)
    sock_m = _SOCKET_ERRORS_RE.search(text)

    return {
        "avg_ms":   round(avg_ms,   3),
        "stdev_ms": round(stdev_ms, 3),
        "max_ms":   round(max_ms,   3),
        "p50_ms":   round(pcts.get("p50_ms", 0.0), 3),
        "p75_ms":   round(pcts.get("p75_ms", 0.0), 3),
        "p90_ms":   round(pcts.get("p90_ms", 0.0), 3),
        "p99_ms":   round(pcts.get("p99_ms", 0.0), 3),
        "total_requests":        int(req_m.group("reqs").replace(",", "")) if req_m else 0,
        "errors_non2xx":         int(err_m.group("n"))        if err_m  else 0,
        "socket_connect_errors": int(sock_m.group("connect")) if sock_m else 0,
        "socket_read_errors":    int(sock_m.group("read"))    if sock_m else 0,
        "socket_write_errors":   int(sock_m.group("write"))   if sock_m else 0,
        "socket_timeout_errors": int(sock_m.group("timeout")) if sock_m else 0,
    }


def _find_wrk2() -> str:
    env = os.environ.get("WRK2")
    if env:
        e = os.path.expanduser(env)
        if os.path.isfile(e) and os.access(e, os.X_OK):
            return e
    for candidate in [
        os.path.expanduser("~/wrk2/wrk"),
        os.path.expanduser("~/wrk2/wrk2"),
    ]:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return "wrk2"


def _run_wrk2(
    wrk2: str,
    lua: str,
    url: str,
    fn_name: str,
    payload_kb: int,
    n_requests: int,
    rate: int,
    timeout_s: int,
) -> tuple[int, str]:
    # Duration chosen so wrk2 at the given rate will send >= n_requests.
    # Add a 5-second buffer for wrk2 warm-up/finish.
    duration_s = max(n_requests // rate + 5, 10)

    env = os.environ.copy()
    env["WRK_PAYLOAD_KB"] = str(payload_kb)
    env["WRK_FUNCTION"]   = fn_name

    cmd = [
        wrk2,
        "-t1",                     # 1 thread
        "-c1",                     # 1 connection slot → sequential requests
        f"-d{duration_s}s",
        f"-R{rate}",
        "--timeout", f"{timeout_s}s",
        "--latency",               # print full percentile distribution
        "-s", lua,
        url,
    ]

    try:
        r = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            timeout=duration_s + timeout_s + 30,
        )
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 binary not found — set WRK2 env var or install in ~/wrk2/wrk"

    output = (r.stdout or "") + (r.stderr or "")
    return r.returncode, output


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Baseline latency: one TCP+TLS connection per request using wrk2."
    )
    parser.add_argument(
        "--mode", choices=["proto", "vanilla"], required=True,
        help="proto = prototype sendfd path; vanilla = standard OpenFaaS proxy chain",
    )
    parser.add_argument("--host", default="192.168.2.2", help="Pi IP address")
    parser.add_argument(
        "--sizes", default="1,5,10,20,30,40,50",
        help="Payload sizes in KB, comma-separated (default: 1,5,10,20,30,40,50)",
    )
    parser.add_argument(
        "--requests", type=int, default=50, dest="n_requests",
        help="Target number of requests per payload size (default: 50)",
    )
    parser.add_argument(
        "--rate", type=int, default=2,
        help="wrk2 constant rate in req/s (default: 2). "
             "With -c1 and Connection:close each request opens a new TLS session. "
             "Duration is set automatically as n_requests/rate + 5s.",
    )
    parser.add_argument("--timeout-s", type=int, default=10, help="Request timeout in seconds")
    parser.add_argument(
        "--function", default=None,
        help="Override function name (default: sumprod-timing-fn-a / sumprod-vanilla-fn-a)",
    )
    parser.add_argument(
        "--scheme", choices=["https", "http"], default="https",
        help="Protocol scheme: https (default, port 8443) or http (port 8080)",
    )
    parser.add_argument(
        "--port", type=int, default=None,
        help="Override port (default: 8443 for https, 8080 for http)",
    )
    parser.add_argument("--output", default="results.csv", help="Output CSV file path")
    args = parser.parse_args()

    fn_name = args.function or {
        "proto":   "sumprod-timing-fn-a",
        "vanilla": "vanilla-fn-a",
    }[args.mode]
    port = args.port or (8443 if args.scheme == "https" else 8080)
    url = f"{args.scheme}://{args.host}:{port}/function/{fn_name}"

    wrk2       = _find_wrk2()
    script_dir = Path(__file__).resolve().parent
    lua        = str(script_dir / "client" / "post_payload_nokeep.lua")
    sizes      = [int(s.strip()) for s in args.sizes.split(",")]

    print(f"mode      : {args.mode}")
    print(f"scheme    : {args.scheme}  (port {port})")
    print(f"url       : {url}")
    print(f"function  : {fn_name}")
    print(f"sizes KB  : {sizes}")
    print(f"requests  : ~{args.n_requests} per size  (rate={args.rate} req/s, -c1, Connection:close)")
    print(f"wrk2      : {wrk2}")
    print(f"lua       : {lua}")
    print(f"output    : {args.output}")
    print()

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    rows = []

    for size_kb in sizes:
        duration_s = max(args.n_requests // args.rate + 5, 10)
        print(
            f"  [{size_kb:>4} KB]  wrk2 -c1 -t1 -R{args.rate} -d{duration_s}s ...",
            end="", flush=True,
        )

        rc, output = _run_wrk2(
            wrk2, lua, url, fn_name,
            payload_kb=size_kb,
            n_requests=args.n_requests,
            rate=args.rate,
            timeout_s=args.timeout_s,
        )
        parsed = _parse_wrk2_output(output)

        server_errors = (
            parsed["errors_non2xx"]
            + parsed["socket_connect_errors"]
            + parsed["socket_read_errors"]
            + parsed["socket_write_errors"]
        )
        warn = f"  ⚠  server_errors={server_errors}" if server_errors else ""

        print(
            f"  avg={parsed['avg_ms']:7.2f}ms"
            f"  stdev={parsed['stdev_ms']:6.2f}ms"
            f"  p99={parsed['p99_ms']:7.2f}ms"
            f"  n={parsed['total_requests']}"
            + warn
        )

        if server_errors:
            print(
                "    !! Server-side errors detected — check function containers on Pi:\n"
                "       sudo ctr -n openfaas-fn task ls"
            )

        rows.append({
            "timestamp":             datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "mode":                  args.mode,
            "scheme":                args.scheme,
            "function":              fn_name,
            "size_kb":               size_kb,
            "n_requests_target":     args.n_requests,
            "rate_rps":              args.rate,
            "total_requests":        parsed["total_requests"],
            "avg_ms":                parsed["avg_ms"],
            "stdev_ms":              parsed["stdev_ms"],
            "max_ms":                parsed["max_ms"],
            "p50_ms":                parsed["p50_ms"],
            "p75_ms":                parsed["p75_ms"],
            "p90_ms":                parsed["p90_ms"],
            "p99_ms":                parsed["p99_ms"],
            "errors_non2xx":         parsed["errors_non2xx"],
            "socket_connect_errors": parsed["socket_connect_errors"],
            "socket_read_errors":    parsed["socket_read_errors"],
            "socket_write_errors":   parsed["socket_write_errors"],
            "socket_timeout_errors": parsed["socket_timeout_errors"],
            "exit_code":             rc,
        })

    fields = list(rows[0].keys())
    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nSaved → {args.output}")


if __name__ == "__main__":
    main()
