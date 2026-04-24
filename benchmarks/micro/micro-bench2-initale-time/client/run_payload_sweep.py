#!/usr/bin/env python3

import argparse
import csv
import http.client
import json
import os
import ssl
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


def parse_sizes(s: str) -> list[int]:
    s = s.strip()
    if not s:
        return []
    out: list[int] = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(int(part))
    return out


def build_sizes(args: argparse.Namespace) -> list[int]:
    if args.sizes:
        return parse_sizes(args.sizes)

    start = args.start_kb * 1024
    end = args.end_kb * 1024
    step = args.step_kb * 1024

    if start <= 0 or end < start or step <= 0:
        raise ValueError("invalid linear payload range")

    return list(range(start, end + 1, step))


def https_request_json(host: str, port: int, path: str, body: bytes, timeout_s: float) -> tuple[int, bytes]:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    conn = http.client.HTTPSConnection(host, port=port, context=ctx, timeout=timeout_s)
    headers = {
        "Content-Type": "application/octet-stream",
        "Connection": "close",
    }

    conn.request("POST", path, body=body, headers=headers)
    resp = conn.getresponse()
    payload = resp.read()
    status = resp.status

    try:
        conn.close()
    except Exception:
        pass

    return status, payload


def main() -> int:
    ap = argparse.ArgumentParser(description="Scenario A (Initial) payload sweep client")
    ap.add_argument(
        "--host",
        required=True,
        help="Proxy host (Raspberry Pi IP/hostname); blank falls back to $PI_IP or 192.168.2.2",
    )
    ap.add_argument("--port", type=int, default=8443, help="Proxy TLS port (default: 8443)")
    ap.add_argument(
        "--path",
        default="/function/timing-fn",
        help="Gateway path to call (default: /function/timing-fn)",
    )
    ap.add_argument(
        "--sizes",
        default="",
        help="Comma-separated payload sizes in bytes; when omitted, a linear KB range is generated",
    )
    ap.add_argument("--start-kb", type=int, default=32, help="First payload size in KiB when --sizes is omitted")
    ap.add_argument("--end-kb", type=int, default=1024, help="Last payload size in KiB when --sizes is omitted")
    ap.add_argument("--step-kb", type=int, default=32, help="Fixed payload step in KiB when --sizes is omitted")
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

    if not sizes:
        print("No sizes specified", file=sys.stderr)
        return 2

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "payload_bytes",
                "iter",
                "http_status",
                "top1_rdtsc",
                "top2_rdtsc",
                "delta_cycles",
                "cntfrq",
                "delta_ns",
                "body_bytes_read",
                "content_length",
                "client_rtt_ns",
            ]
        )

        for size in sizes:
            body = b"a" * size
            for i in range(args.requests):
                t_start = time.time_ns()
                try:
                    status, raw = https_request_json(
                        args.host, args.port, args.path, body, timeout_s=args.timeout
                    )
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
                        except json.JSONDecodeError:
                            pass

                    w.writerow(
                        [
                            size,
                            i,
                            status,
                            top1_rdtsc,
                            top2_rdtsc,
                            delta_cycles,
                            cntfrq,
                            delta_ns,
                            body_bytes_read,
                            content_length,
                            t_end - t_start,
                        ]
                    )
                except Exception as e:
                    t_end = time.time_ns()
                    print(f"error payload={size}B iter={i}: {e}", file=sys.stderr)
                    w.writerow([size, i, "ERR", "", "", "", "", "", "", "", t_end - t_start])

            print(f"done payload={size}B", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
