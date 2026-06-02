#!/usr/bin/env python3
"""
run_keepalive_sweep.py — Core keep-alive HTTP payload sweep client.

Integration bench variant: all requests go through the single OpenFaaS
gateway on port 8080, which dispatches via SCM_RIGHTS (prototype mode) or
standard HTTP proxy (vanilla mode).

The client uses ONE persistent TCP connection per payload size and
alternates requests between fn-a and fn-b (mode=switch), measuring the
end-to-end latency including the FD transfer overhead.

Usage (called from run_proto_evaluation.py or run_vanilla_evaluation.py):
    python3 run_keepalive_sweep.py \
        --host 192.168.2.2 --port 8080 \
        --fn-a /function/timing-fn-a --fn-b /function/timing-fn-b \
        --mode switch \
        --start-kb 32 --end-kb 1024 --step-kb 32 \
        --requests 50 --out results.csv
"""

import argparse
import csv
import http.client
import json
import os
import sys
import time

# ── Default configuration ─────────────────────────────────────────────────────

DEFAULT_PI_HOST = "192.168.2.2"

# ── Helpers ───────────────────────────────────────────────────────────────────

def resolve_host(host: str) -> str:
    """Return host, falling back to $PI_IP env var, then to the default Pi IP."""
    host = host.strip()
    if host:
        return host
    env_host = os.environ.get("PI_IP", "").strip()
    if env_host:
        return env_host
    return DEFAULT_PI_HOST


def build_sizes(args: argparse.Namespace) -> list[int]:
    """Build the list of payload sizes in bytes from the CLI arguments."""
    start = args.start_kb * 1024
    end   = args.end_kb   * 1024
    step  = args.step_kb  * 1024
    if start <= 0 or end < start or step <= 0:
        raise ValueError("invalid linear payload range")
    return list(range(start, end + 1, step))


# ── Main sweep loop ───────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Keep-Alive HTTP payload sweep — integration bench (port 8080)"
    )
    # ── Connection ────────────────────────────────────────────────────────────
    ap.add_argument("--host",    default="",   help="Gateway host (Pi IP)")
    ap.add_argument("--port",    type=int, default=8080,
                    help="Gateway HTTP port (default: 8080 — the unified OpenFaaS gateway)")

    # ── Function paths ────────────────────────────────────────────────────────
    ap.add_argument("--fn-a",   default="/function/timing-fn-a",
                    help="URL path for function A (prototype) or vanilla-fn-a (vanilla)")
    ap.add_argument("--fn-b",   default="/function/timing-fn-b",
                    help="URL path for function B (prototype) or vanilla-fn-b (vanilla)")

    # ── Mode ──────────────────────────────────────────────────────────────────
    ap.add_argument("--mode", choices=["single", "switch"], default="switch",
                    help="'switch' alternates fn-a/fn-b on the same connection (default); "
                         "'single' always uses fn-a")

    # ── Payload range ─────────────────────────────────────────────────────────
    ap.add_argument("--start-kb", type=int, default=32,   help="First payload size in KiB")
    ap.add_argument("--end-kb",   type=int, default=1024, help="Last  payload size in KiB")
    ap.add_argument("--step-kb",  type=int, default=32,   help="Payload step in KiB")

    # ── Repetitions & timeout ─────────────────────────────────────────────────
    ap.add_argument("--requests", type=int,   default=50,   help="Requests per payload size")
    ap.add_argument("--timeout",  type=float, default=30.0, help="Per-request timeout (s)")

    # ── Output ────────────────────────────────────────────────────────────────
    ap.add_argument("--out", required=True, help="Output CSV file path")

    args = ap.parse_args()
    args.host = resolve_host(args.host)

    try:
        sizes = build_sizes(args)
    except ValueError as exc:
        print(f"Invalid payload configuration: {exc}", file=sys.stderr)
        return 2

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        # CSV columns — identical schema to micro-bench3-keepalive-http
        w.writerow([
            "payload_bytes", "iter", "http_status",
            "top1_rdtsc", "top2_rdtsc",
            "delta_cycles", "cntfrq", "delta_ns",
            "body_bytes_read", "content_length",
            "client_rtt_ns", "target_fn",
        ])

        for size in sizes:
            body = b"a" * size

            # One persistent TCP connection per payload size
            conn = http.client.HTTPConnection(args.host, port=args.port, timeout=args.timeout)

            for i in range(args.requests):
                # Alternate between fn-a and fn-b on the SAME connection
                if args.mode == "switch":
                    target_path = args.fn_a if i % 2 == 0 else args.fn_b
                else:
                    target_path = args.fn_a

                # Last request closes the connection; all others keep it alive
                is_last = (i == args.requests - 1)
                headers = {
                    "Content-Type": "application/octet-stream",
                    "Connection": "close" if is_last else "keep-alive",
                }

                t_start = time.time_ns()
                try:
                    conn.request("POST", target_path, body=body, headers=headers)
                    resp = conn.getresponse()
                    raw  = resp.read()
                    status = resp.status
                    t_end = time.time_ns()

                    # Parse the JSON timing payload returned by the C worker
                    top1_rdtsc = top2_rdtsc = delta_cycles = ""
                    cntfrq = delta_ns = body_bytes_read = content_length = ""

                    if raw:
                        try:
                            data = json.loads(raw.decode("utf-8", errors="replace"))
                            top1_rdtsc     = data.get("top1_rdtsc",     "")
                            top2_rdtsc     = data.get("top2_rdtsc",     "")
                            delta_cycles   = data.get("delta_cycles",   "")
                            cntfrq         = data.get("cntfrq",         "")
                            delta_ns       = data.get("delta_ns",       "")
                            body_bytes_read = data.get("body_bytes_read", "")
                            content_length = data.get("content_length", "")
                            # Vanilla uses Transfer-Encoding: chunked → content_length=0
                            if content_length == 0 and body_bytes_read != "":
                                content_length = body_bytes_read
                        except json.JSONDecodeError:
                            pass

                    w.writerow([
                        size, i, status,
                        top1_rdtsc, top2_rdtsc,
                        delta_cycles, cntfrq, delta_ns,
                        body_bytes_read, content_length,
                        t_end - t_start, target_path,
                    ])

                except Exception as e:
                    t_end = time.time_ns()
                    print(f"  error payload={size}B iter={i}: {e}", file=sys.stderr)
                    w.writerow([size, i, "ERR", "", "", "", "", "", "", "", t_end - t_start, target_path])
                    # Reconnect after any error
                    conn.close()
                    conn = http.client.HTTPConnection(args.host, port=args.port, timeout=args.timeout)

            conn.close()
            print(f"  done payload={size // 1024}KiB ({size}B)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
