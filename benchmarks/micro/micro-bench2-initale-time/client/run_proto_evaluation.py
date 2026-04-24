#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from pathlib import Path


DEFAULT_PI_HOST = "192.168.2.2"


def resolve_host(host: str) -> str:
    host = host.strip()
    if host:
        return host

    env_host = os.environ.get("PI_IP", "").strip()
    if env_host:
        return env_host

    return DEFAULT_PI_HOST


def run_sweep(
    runner: Path,
    host: str,
    port: int,
    requests: int,
    timeout: float,
    start_kb: int,
    end_kb: int,
    step_kb: int,
    out_csv: Path,
    label: str,
) -> None:
    host = resolve_host(host)

    print(f"[run] {label}")
    print(
        "[run] "
        f"host={host} port={port} start_kb={start_kb} end_kb={end_kb} "
        f"step_kb={step_kb} requests={requests} timeout={timeout}"
    )

    cmd = [
        sys.executable,
        str(runner),
        "--host",
        host,
        "--port",
        str(port),
        "--start-kb",
        str(start_kb),
        "--end-kb",
        str(end_kb),
        "--step-kb",
        str(step_kb),
        "--requests",
        str(requests),
        "--timeout",
        str(timeout),
        "--out",
        str(out_csv),
    ]
    subprocess.run(cmd, check=True)
    print(f"[ok] wrote {out_csv}")


def main() -> int:
    base_dir = Path(__file__).resolve().parent
    runner = base_dir / "run_payload_sweep.py"

    parser = argparse.ArgumentParser(
        description="Run the three prototype evaluation payload ranges for micro-bench2-initale-time"
    )
    parser.add_argument(
        "--host",
        required=True,
        help="Raspberry Pi IP/hostname; blank falls back to $PI_IP or 192.168.2.2",
    )
    parser.add_argument("--port", type=int, default=9443, help="Prototype gateway TLS port")
    parser.add_argument("--requests", type=int, default=50, help="Requests per payload size")
    parser.add_argument("--timeout", type=float, default=120.0, help="Per-request timeout in seconds")
    parser.add_argument(
        "--output-dir",
        default=str(base_dir),
        help="Directory where the CSV files will be written",
    )
    parser.add_argument(
        "--only",
        choices=["step32", "step100", "step1000", "all"],
        default="all",
        help="Run only one sweep set or all of them",
    )
    parser.add_argument("--start-kb", type=int, help="Custom first payload size in KiB")
    parser.add_argument("--end-kb", type=int, help="Custom last payload size in KiB")
    parser.add_argument("--step-kb", type=int, help="Custom payload step in KiB")
    parser.add_argument("--out", help="Custom output CSV path")
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    custom_range_requested = any(
        value is not None for value in (args.start_kb, args.end_kb, args.step_kb, args.out)
    )
    if custom_range_requested:
        if None in (args.start_kb, args.end_kb, args.step_kb) or not args.out:
            parser.error("custom mode requires --start-kb, --end-kb, --step-kb, and --out")
        run_sweep(
            runner=runner,
            host=args.host,
            port=args.port,
            requests=args.requests,
            timeout=args.timeout,
            start_kb=args.start_kb,
            end_kb=args.end_kb,
            step_kb=args.step_kb,
            out_csv=Path(args.out).resolve(),
            label="custom prototype sweep",
        )
        print("[ok] prototype evaluation completed")
        return 0

    sweeps = [
        (
            "step32",
            "step 32 KiB, 32 KiB -> 1024 KiB",
            32,
            1024,
            32,
            output_dir / "proto_results_step32kb_32_to_1024.csv",
        ),
        (
            "step100",
            "step 100 KiB, 1000 KiB -> 1500 KiB",
            1000,
            1500,
            100,
            output_dir / "proto_results_step100kb_1000_to_1500.csv",
        ),
        (
            "step1000",
            "step 1000 KiB, 1500 KiB -> 60500 KiB",
            1500,
            60500,
            1000,
            output_dir / "proto_results_step1000kb_1500_to_60500.csv",
        ),
    ]

    for key, label, start_kb, end_kb, step_kb, out_csv in sweeps:
        if args.only != "all" and args.only != key:
            continue
        run_sweep(
            runner=runner,
            host=args.host,
            port=args.port,
            requests=args.requests,
            timeout=args.timeout,
            start_kb=start_kb,
            end_kb=end_kb,
            step_kb=step_kb,
            out_csv=out_csv,
            label=label,
        )

    print("[ok] prototype evaluation completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())