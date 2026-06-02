#!/usr/bin/env python3
"""
run_proto_evaluation.py — Run the PROTOTYPE evaluation sweep.

Prerequisites on the Pi (before running this script):
  1. The gateway must be in PROTOTYPE mode:
       HTTPMIGRATE_ENABLE=1 in /var/lib/faasd/docker-compose.yaml → restart faasd
  2. The prototype functions must be deployed:
       faas-cli deploy -f deploy/timing-fn-a.yml && sleep 3
       faas-cli deploy -f deploy/timing-fn-b.yml && sleep 3
  3. Verify both are RUNNING:
       ssh romero@192.168.2.2 "sudo ctr -n openfaas-fn task list"

Usage (run from the repo root on your building machine):
    python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/run_proto_evaluation.py \
        --host 192.168.2.2

Output CSV files written to: evaluation/results/
    proto_results_step32kb_32_to_1024.csv
    proto_results_step100kb_1000_to_1500.csv
"""

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
    fn_a: str,
    fn_b: str,
    requests: int,
    timeout: float,
    start_kb: int,
    end_kb: int,
    step_kb: int,
    out_csv: Path,
    label: str,
) -> None:
    host = resolve_host(host)
    print(f"\n[proto] {label}")
    print(f"[proto] host={host}:{port}  {start_kb}KiB→{end_kb}KiB step={step_kb}KiB  {requests} req/size")

    cmd = [
        sys.executable, str(runner),
        "--host",     host,
        "--port",     str(port),
        "--fn-a",     fn_a,
        "--fn-b",     fn_b,
        "--mode",     "switch",   # alternate fn-a/fn-b on the same connection
        "--start-kb", str(start_kb),
        "--end-kb",   str(end_kb),
        "--step-kb",  str(step_kb),
        "--requests", str(requests),
        "--timeout",  str(timeout),
        "--out",      str(out_csv),
    ]
    subprocess.run(cmd, check=True)
    print(f"[proto] ✓ wrote {out_csv}")


def main() -> int:
    base_dir    = Path(__file__).resolve().parent
    runner      = base_dir / "run_keepalive_sweep.py"
    results_dir = base_dir / "results"

    parser = argparse.ArgumentParser(
        description="Run the PROTOTYPE (SCM_RIGHTS sendfd) evaluation sweeps"
    )
    parser.add_argument(
        "--host", required=True,
        help="Raspberry Pi IP/hostname (e.g. 192.168.2.2); falls back to $PI_IP",
    )
    # Gateway port — prototype and vanilla both use the same unified gateway on :8080
    parser.add_argument("--port",     type=int,   default=8080,   help="Gateway HTTP port (default: 8080)")
    parser.add_argument("--requests", type=int,   default=50,     help="Requests per payload size")
    parser.add_argument("--timeout",  type=float, default=60.0,   help="Per-request timeout in seconds")
    parser.add_argument(
        "--output-dir", default=str(results_dir),
        help="Directory where CSV files are written (default: evaluation/results/)",
    )
    parser.add_argument(
        "--only",
        choices=["step32", "step100", "all"],
        default="all",
        help="Run only one sweep range or all (default: all)",
    )
    # Custom single sweep override
    parser.add_argument("--start-kb", type=int, help="Custom start payload size (KiB)")
    parser.add_argument("--end-kb",   type=int, help="Custom end   payload size (KiB)")
    parser.add_argument("--step-kb",  type=int, help="Custom payload step       (KiB)")
    parser.add_argument("--out",                help="Custom output CSV path")
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Function paths (prototype mode)
    fn_a = "/function/timing-fn-a"
    fn_b = "/function/timing-fn-b"

    # ── Custom single-range mode ──────────────────────────────────────────────
    custom_range_requested = any(v is not None for v in (args.start_kb, args.end_kb, args.step_kb, args.out))
    
    if custom_range_requested:
        # Fallback to standard defaults if only some are set
        start = args.start_kb if args.start_kb is not None else 32
        end = args.end_kb if args.end_kb is not None else 1024
        step = args.step_kb if args.step_kb is not None else 32
        
        if args.out:
            out_path = Path(args.out).resolve()
        else:
            out_path = output_dir / f"proto_results_custom_{start}_to_{end}_step{step}.csv"
            
        run_sweep(
            runner=runner, host=args.host, port=args.port,
            fn_a=fn_a, fn_b=fn_b,
            requests=args.requests, timeout=args.timeout,
            start_kb=start, end_kb=end, step_kb=step,
            out_csv=out_path,
            label=f"custom prototype sweep ({start}KiB -> {end}KiB step {step}KiB)",
        )
        print("[proto] evaluation completed ✓")
        return 0

    # ── Standard sweep sets ───────────────────────────────────────────────────
    # Same payload ranges as micro-bench3-keepalive-http for direct comparison.
    sweeps = [
        (
            "step32",
            "step=32KiB, range 32KiB→1024KiB",
            32, 1024, 32,
            output_dir / "proto_results_step32kb_32_to_1024.csv",
        ),
        (
            "step100",
            "step=100KiB, range 1000KiB→1500KiB",
            1000, 1500, 100,
            output_dir / "proto_results_step100kb_1000_to_1500.csv",
        ),
    ]

    for key, label, start_kb, end_kb, step_kb, out_csv in sweeps:
        if args.only != "all" and args.only != key:
            continue
        run_sweep(
            runner=runner, host=args.host, port=args.port,
            fn_a=fn_a, fn_b=fn_b,
            requests=args.requests, timeout=args.timeout,
            start_kb=start_kb, end_kb=end_kb, step_kb=step_kb,
            out_csv=out_csv, label=label,
        )

    print("\n[proto] ALL evaluation sweeps completed ✓")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
