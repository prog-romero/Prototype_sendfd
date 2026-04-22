#!/usr/bin/env python3
"""
run_combined_sweep.py — Payload sweep for forward-TCP vs migration.

For every payload size, the client opens one fresh keepalive connection,
sends one priming request to function A, then sends a fixed alternating
sequence B, A, B, A, ... for warmup and measurement.

This gives the intended behaviour for the new benchmark:
    - vanilla: every measured request follows the same post-decrypt forward path
    - prototype: every measured request arrives first on the wrong owner and is
        migrated to the right owner

Only measured requests are written to the CSV. The priming request is never
recorded.
"""

import argparse
import csv
import http.client
import json
import os
import random
import ssl
import sys

# ─── Fixed sweep parameters ────────────────────────────────────────────────

# (payload_size_bytes, num_requests, warmup)
# More requests for small payloads (fast), fewer for large (slow).
DEFAULT_PAYLOAD_SCHEDULE = [
    (64,          20,  5),   #  64 B   — very fast, many samples
    (256,         20,  5),   # 256 B
    (1_024,        8,  3),   #   1 KB
    (4_096,        6,  2),   #   4 KB
    (16_384,       4,  2),   #  16 KB
    (65_536,       4,  1),   #  64 KB
    (131_072,      4,  1),   # 128 KB
    (262_144,      3,  1),   # 256 KB
    (524_288,      2,  1),   # 512 KB
    (1_048_576,    2,  1),   #   1 MB
]


def build_payload_schedule(num_requests_per_payload: int | None,
                           warmup_per_payload: int | None) -> list[tuple[int, int, int]]:
    if num_requests_per_payload is None and warmup_per_payload is None:
        return list(DEFAULT_PAYLOAD_SCHEDULE)

    if num_requests_per_payload is None:
        return [(size, reqs, warmup_per_payload) for size, reqs, _ in DEFAULT_PAYLOAD_SCHEDULE]

    if warmup_per_payload is None:
        warmup_per_payload = min(20, max(1, num_requests_per_payload // 10))

    payload_sizes = [size for size, _, _ in DEFAULT_PAYLOAD_SCHEDULE]
    return [(size, num_requests_per_payload, warmup_per_payload) for size in payload_sizes]


# ─── SSL / connection helpers ──────────────────────────────────────────────

def _make_ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode    = ssl.CERT_NONE
    return ctx


def _make_conn(host: str, port: int, timeout: float) -> http.client.HTTPSConnection:
    return http.client.HTTPSConnection(
        host, port=port, context=_make_ssl_ctx(), timeout=timeout
    )


# ─── Single request ────────────────────────────────────────────────────────

def do_request(conn: http.client.HTTPSConnection,
               fn_name: str,
               payload: bytes) -> dict:
    headers = {
        "Content-Type":   "application/octet-stream",
        "Content-Length": str(len(payload)),
        "Connection":     "keep-alive",
    }
    conn.request("POST", f"/function/{fn_name}", body=payload, headers=headers)
    resp = conn.getresponse()
    return json.loads(resp.read().decode())


def build_alternating_schedule(fn_a: str, fn_b: str, n_total: int, first_target: str) -> list[str]:
    if n_total <= 0:
        return []

    schedule: list[str] = []
    current = first_target
    for _ in range(n_total):
        schedule.append(current)
        current = fn_a if current == fn_b else fn_b
    return schedule


# ─── Core sweep ───────────────────────────────────────────────────────────

def run_sweep(host: str, port: int,
              fn_a: str, fn_b: str,
              payload_schedule: list[tuple[int, int, int]],
              timeout: float,
              label: str,
              out_path: str,
              seed: int = 42) -> None:
    all_rows: list[dict] = []

    random.Random(seed)
    total_runs = len(payload_schedule)
    run_no     = 0

    for payload_size, num_requests, warmup in payload_schedule:
        run_no += 1
        print(
            f"\n[{run_no:2d}/{total_runs}] payload={payload_size:>10,} B "
            f"({num_requests} measured + {warmup} warmup + 1 prime)"
        )

        payload = os.urandom(payload_size)
        measure_sched = build_alternating_schedule(fn_a, fn_b, num_requests, fn_b)
        warmup_sched = build_alternating_schedule(fn_a, fn_b, warmup, fn_b)
        errors = 0

        conn = _make_conn(host, port, timeout)
        try:
            do_request(conn, fn_a, payload)
        except Exception as exc:
            try:
                conn.close()
            except Exception:
                pass
            print(f"  prime error: {exc}", file=sys.stderr)
            continue

        for fn in warmup_sched:
            try:
                do_request(conn, fn, payload)
            except Exception as exc:
                errors += 1
                print(f"  warmup error: {exc}", file=sys.stderr)
                break

        if errors:
            try:
                conn.close()
            except Exception:
                pass
            print("  warmup failed; skipping measured requests", file=sys.stderr)
            continue

        prev_fn = fn_a if not warmup_sched else warmup_sched[-1]
        for seq_i, fn in enumerate(measure_sched):
            switched = int(fn != prev_fn)
            try:
                data = do_request(conn, fn, payload)
            except Exception as exc:
                errors += 1
                print(f"  req {seq_i} error: {exc}", file=sys.stderr)
                prev_fn = fn
                break

            cntfrq = data.get("cntfrq", 1) or 1
            delta_cycles = data.get("delta_cycles", 0)
            all_rows.append({
                "label": label,
                "alpha": 0,
                "payload_size": payload_size,
                "request_seq": seq_i + 1,
                "request_no": data.get("request_no", seq_i + 1),
                "worker": data.get("worker", ""),
                "target_fn": fn,
                "switched": switched,
                "delta_cycles": delta_cycles,
                "cntfrq": cntfrq,
                "delta_ns": int(delta_cycles * 1_000_000_000 / cntfrq),
                "requests_per_conn": num_requests + warmup + 1,
                "top1_rdtsc": data.get("top1_rdtsc", 0),
                "top2_rdtsc": data.get("top2_rdtsc", 0),
            })
            prev_fn = fn

        try:
            conn.close()
        except Exception:
            pass

        ok = num_requests - errors
        print(f"  done — {ok} ok, {errors} errors")

    # ── write combined CSV ───────────────────────────────────────────────
    fieldnames = [
        "label", "alpha", "payload_size",
        "request_seq", "request_no",
        "worker", "target_fn", "switched",
        "delta_cycles", "cntfrq", "delta_ns",
        "requests_per_conn", "top1_rdtsc", "top2_rdtsc",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(all_rows)

    expected = sum(n for _, n, _ in payload_schedule)
    print(f"\nWrote {len(all_rows)}/{expected} rows → {out_path}")


# ─── main ─────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Payload sweep for forward-TCP vs migration",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host",              default="192.168.2.2")
    ap.add_argument("--port",              type=int, default=8444,
                    help="8444=vanilla forward path / 9444=prototype migration path")
    ap.add_argument("--function-a",        default="bench2-fn-a")
    ap.add_argument("--function-b",        default="bench2-fn-b")
    ap.add_argument("--timeout",           type=float, default=30.0)
    ap.add_argument("--label",             default="vanilla",
                    help="label written into the CSV (vanilla or proto)")
    ap.add_argument("--seed",              type=int, default=42,
                    help="retained for reproducibility; the request schedule is deterministic")
    ap.add_argument("--num-requests-per-payload", type=int, default=None,
                    help="override the mixed schedule and use the same request count for every payload size")
    ap.add_argument("--warmup-per-payload", type=int, default=None,
                    help="override the warmup count for every payload size")
    ap.add_argument("--out",               required=True,
                    help="output CSV path, e.g. results/combined_vanilla.csv")
    args = ap.parse_args()

    if args.num_requests_per_payload is not None and args.num_requests_per_payload <= 0:
        print("error: --num-requests-per-payload must be > 0", file=sys.stderr)
        return 2
    if args.warmup_per_payload is not None and args.warmup_per_payload < 0:
        print("error: --warmup-per-payload must be >= 0", file=sys.stderr)
        return 2

    payload_schedule = build_payload_schedule(
        num_requests_per_payload=args.num_requests_per_payload,
        warmup_per_payload=args.warmup_per_payload,
    )

    total_rows = sum(n for _, n, _ in payload_schedule)
    print(f"Payload schedule (size, requests, warmup):")
    for size, nreq, nwarm in payload_schedule:
        print(f"  {size:>10,} B  →  {nreq:4d} req  + {nwarm} warmup")
    print("\nSchedule: 1 prime request to function-a, then alternating measured requests")
    print(f"Total runs: {len(payload_schedule)}")
    print(f"Total rows expected: {total_rows:,}")
    print("Requests per TLS conn: one fresh keepalive connection per payload size")
    print(f"Label: {args.label}")
    print(f"Output: {args.out}")

    run_sweep(
        host=args.host,
        port=args.port,
        fn_a=args.function_a,
        fn_b=args.function_b,
        payload_schedule=payload_schedule,
        timeout=args.timeout,
        label=args.label,
        out_path=args.out,
        seed=args.seed,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
