#!/usr/bin/env python3
"""
run_keepalive_sweep.py — Keep-Alive HTTP sweep client for the integration bench.

Connects to the REAL OpenFaaS gateway (:8080) instead of the standalone
prototype gateway (:8082).  All other CLI options and the output CSV format
are identical to the micro-bench3-keepalive-http client so results can be
compared directly.

CSV columns (identical to the prototype bench):
  payload_bytes, iter, http_status, top1_rdtsc, top2_rdtsc,
  delta_cycles, cntfrq, delta_ns, body_bytes_read, content_length,
  client_rtt_ns, target_fn
"""

import argparse
import csv
import http.client
import json
import os
import sys
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080  # real OpenFaaS gateway


def resolve_host(host: str) -> str:
    host = host.strip()
    if host:
        return host
    env_host = os.environ.get("GATEWAY_IP", os.environ.get("PI_IP", "")).strip()
    if env_host:
        return env_host
    return DEFAULT_HOST


def build_sizes(args: argparse.Namespace) -> list[int]:
    start = args.start_kb * 1024
    end   = args.end_kb   * 1024
    step  = args.step_kb  * 1024
    if start <= 0 or end < start or step <= 0:
        raise ValueError("invalid linear payload range")
    return list(range(start, end + 1, step))


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Keep-Alive HTTP payload sweep — integration bench (real gateway)")
    ap.add_argument("--host",     default="",
                    help="Gateway host (default: 127.0.0.1 or $GATEWAY_IP)")
    ap.add_argument("--port",     type=int, default=DEFAULT_PORT,
                    help=f"Gateway HTTP port (default: {DEFAULT_PORT})")
    ap.add_argument("--mode",     choices=["single", "switch"], default="switch",
                    help="'single' (same fn) or 'switch' (alternating)")
    ap.add_argument("--fn-a",     default="/function/timing-fn-a",
                    help="Path for function A")
    ap.add_argument("--fn-b",     default="/function/timing-fn-b",
                    help="Path for function B")
    ap.add_argument("--start-kb", type=int, default=32,
                    help="First payload size in KiB (default: 32)")
    ap.add_argument("--end-kb",   type=int, default=2048,
                    help="Last payload size in KiB (default: 2048)")
    ap.add_argument("--step-kb",  type=int, default=32,
                    help="Payload step in KiB (default: 32)")
    ap.add_argument("--requests", type=int, default=50,
                    help="Requests per payload size (default: 50)")
    ap.add_argument("--timeout",  type=float, default=10.0,
                    help="Per-request socket timeout in seconds (default: 10)")
    ap.add_argument("--out",      required=True,
                    help="Output CSV path")
    args = ap.parse_args()
    args.host = resolve_host(args.host)

    try:
        sizes = build_sizes(args)
    except ValueError as exc:
        print(f"Invalid payload configuration: {exc}", file=sys.stderr)
        return 2

    print(f"Gateway: http://{args.host}:{args.port}  mode={args.mode}  "
          f"sizes={len(sizes)}  requests_per_size={args.requests}",
          file=sys.stderr)

    with open(args.out, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "payload_bytes", "iter", "http_status",
            "top1_rdtsc", "top2_rdtsc", "delta_cycles",
            "cntfrq", "delta_ns", "body_bytes_read",
            "content_length", "client_rtt_ns", "target_fn",
        ])

        for size in sizes:
            body = b"a" * size

            conn = http.client.HTTPConnection(
                args.host, port=args.port, timeout=args.timeout)

            for i in range(args.requests):
                if args.mode == "switch":
                    target_path = args.fn_a if i % 2 == 0 else args.fn_b
                else:
                    target_path = args.fn_a

                is_last = (i == args.requests - 1)
                headers = {
                    "Content-Type": "application/octet-stream",
                    "Connection":   "close" if is_last else "keep-alive",
                }

                t_start = time.time_ns()
                try:
                    conn.request("POST", target_path, body=body, headers=headers)
                    resp     = conn.getresponse()
                    raw      = resp.read()
                    status   = resp.status
                    t_end    = time.time_ns()

                    top1_rdtsc     = ""
                    top2_rdtsc     = ""
                    delta_cycles   = ""
                    cntfrq_val     = ""
                    delta_ns       = ""
                    body_bytes_read = ""
                    content_length = ""

                    if raw:
                        try:
                            data = json.loads(raw.decode("utf-8", errors="replace"))
                            top1_rdtsc      = data.get("top1_rdtsc",      "")
                            top2_rdtsc      = data.get("top2_rdtsc",      "")
                            delta_cycles    = data.get("delta_cycles",    "")
                            cntfrq_val      = data.get("cntfrq",          "")
                            delta_ns        = data.get("delta_ns",        "")
                            body_bytes_read = data.get("body_bytes_read", "")
                            content_length  = data.get("content_length",  "")
                            if content_length == 0 and body_bytes_read != "":
                                content_length = body_bytes_read
                        except json.JSONDecodeError:
                            pass

                    writer.writerow([
                        size, i, status,
                        top1_rdtsc, top2_rdtsc, delta_cycles,
                        cntfrq_val, delta_ns, body_bytes_read,
                        content_length, t_end - t_start, target_path,
                    ])

                except Exception as exc:
                    t_end = time.time_ns()
                    print(f"error payload={size}B iter={i}: {exc}", file=sys.stderr)
                    writer.writerow([
                        size, i, "ERR", "", "", "", "", "", "", "",
                        t_end - t_start, target_path,
                    ])
                    conn.close()
                    conn = http.client.HTTPConnection(
                        args.host, port=args.port, timeout=args.timeout)

            conn.close()
            print(f"done payload={size}B", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
