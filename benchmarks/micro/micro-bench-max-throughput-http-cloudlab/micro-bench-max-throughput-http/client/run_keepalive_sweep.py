#!/usr/bin/env python3

import argparse
import csv
import http.client
import json
import os
import sys
import time

DEFAULT_PI_HOST = "192.168.2.2"

def resolve_host(host: str) -> str:
    host = host.strip()
    if host:
        return host

    env_host = os.environ.get("PI_IP", "").strip()
    if env_host:
        return env_host

    return DEFAULT_PI_HOST

def build_sizes(args: argparse.Namespace) -> list[int]:
    start = args.start_kb * 1024
    end = args.end_kb * 1024
    step = args.step_kb * 1024

    if start <= 0 or end < start or step <= 0:
        raise ValueError("invalid linear payload range")

    return list(range(start, end + 1, step))

def main() -> int:
    ap = argparse.ArgumentParser(description="Scenario B (Keep-Alive) HTTP payload sweep client")
    ap.add_argument("--host", default="", help="Gateway host")
    ap.add_argument("--port", type=int, default=8082, help="Gateway HTTP port (default: 8082)")
    ap.add_argument("--mode", choices=["single", "switch"], default="switch", 
                    help="Mode: 'single' (same function) or 'switch' (alternating functions)")
    ap.add_argument("--fn-a", default="/function/timing-fn-a", help="Path for function A")
    ap.add_argument("--fn-b", default="/function/timing-fn-b", help="Path for function B")
    ap.add_argument("--start-kb", type=int, default=32, help="First payload size in KiB")
    ap.add_argument("--end-kb", type=int, default=2048, help="Last payload size in KiB")
    ap.add_argument("--step-kb", type=int, default=32, help="Fixed payload step in KiB")
    ap.add_argument("--requests", type=int, default=50, help="Requests per payload size")
    ap.add_argument("--timeout", type=float, default=10.0, help="Per-request timeout (s)")
    ap.add_argument("--out", required=True, help="Output CSV path")
    args = ap.parse_args()
    args.host = resolve_host(args.host)

    try:
        sizes = build_sizes(args)
    except ValueError as exc:
        print(f"Invalid payload configuration: {exc}", file=sys.stderr)
        return 2

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "payload_bytes", "iter", "http_status", "top1_rdtsc", "top2_rdtsc",
            "delta_cycles", "cntfrq", "delta_ns", "body_bytes_read",
            "content_length", "client_rtt_ns", "target_fn"
        ])

        for size in sizes:
            body = b"a" * size
            
            # Use a persistent connection for Keep-Alive
            conn = http.client.HTTPConnection(args.host, port=args.port, timeout=args.timeout)
            
            for i in range(args.requests):
                if args.mode == "switch":
                    target_path = args.fn_a if i % 2 == 0 else args.fn_b
                else:
                    target_path = args.fn_a

                # Last request of the iteration closes the connection, others keep it alive
                is_last = (i == args.requests - 1)
                headers = {
                    "Content-Type": "application/octet-stream",
                    "Connection": "close" if is_last else "keep-alive",
                }

                t_start = time.time_ns()
                try:
                    conn.request("POST", target_path, body=body, headers=headers)
                    resp = conn.getresponse()
                    raw = resp.read()
                    status = resp.status
                    t_end = time.time_ns()

                    top1_rdtsc = ""
                    top2_rdtsc = ""
                    delta_cycles = ""
                    cntfrq = ""
                    delta_ns = ""
                    body_bytes_read = ""
                    content_length = ""

                    if raw:
                        try:
                            data = json.loads(raw.decode("utf-8", errors="replace"))
                            top1_rdtsc = data.get("top1_rdtsc", "")
                            top2_rdtsc = data.get("top2_rdtsc", "")
                            delta_cycles = data.get("delta_cycles", "")
                            cntfrq = data.get("cntfrq", "")
                            delta_ns = data.get("delta_ns", "")
                            body_bytes_read = data.get("body_bytes_read", "")
                            content_length = data.get("content_length", "")

                            if content_length == 0 and body_bytes_read != "":
                                content_length = body_bytes_read
                        except json.JSONDecodeError:
                            pass

                    w.writerow([
                        size, i, status, top1_rdtsc, top2_rdtsc,
                        delta_cycles, cntfrq, delta_ns, body_bytes_read,
                        content_length, t_end - t_start, target_path
                    ])
                except Exception as e:
                    t_end = time.time_ns()
                    print(f"error payload={size}B iter={i}: {e}", file=sys.stderr)
                    w.writerow([size, i, "ERR", "", "", "", "", "", "", "", t_end - t_start, target_path])
                    # Re-establish connection on error
                    conn.close()
                    conn = http.client.HTTPConnection(args.host, port=args.port, timeout=args.timeout)

            conn.close()
            print(f"done payload={size}B", file=sys.stderr)

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
