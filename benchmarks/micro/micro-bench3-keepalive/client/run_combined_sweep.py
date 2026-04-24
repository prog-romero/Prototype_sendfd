#!/usr/bin/env python3
"""
run_combined_sweep.py — Combined sweep benchmark client for keepalive benches.

By default it sweeps alpha ∈ {0, 25, 50} × payload_size ∈ {10 fixed sizes,
64 B → 1 MB}. You can override the alpha list, including the special value
100, to generate one-alpha result files for fairness checks.

Every combination runs a fixed number of requests at the same payload size.
By default the schedule uses more samples for small payloads and fewer for
large ones, but you can override it and force the same request count for
every payload size when you want a perfectly uniform per-payload comparison.

All results are written into ONE CSV so vanilla and proto can be compared
side-by-side or on matched alpha slices.

Total combinations: 3 alpha × 10 sizes = 30 runs × ~68 requests avg = ~2040 rows.

━━━ Schedule per (alpha, payload_size) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    alpha=0   → strictly alternating: A B A B A B …
    alpha=25  → [25% consecutive to fn-a] [25% consecutive to fn-b] [50% random]
    alpha=50  → [50% consecutive to fn-a] [50% consecutive to fn-b] (no random)
    alpha=100 → all requests pinned to function-a

  Every request in a given run sends exactly the SAME fixed payload size.

━━━ Usage ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    # Vanilla evaluation
  python3 run_combined_sweep.py \\
      --host 192.168.2.2 --port 8444 \\
    --label vanilla \
    --requests-per-conn 50 \
      --out results/combined_vanilla.csv

  # Prototype evaluation (identical parameters, only port and label change)
  python3 run_combined_sweep.py \\
      --host 192.168.2.2 --port 9444 \\
      --label proto \
      --requests-per-conn 50 \
      --out results/combined_proto.csv

  # Bench3 alpha=0 single-step evaluation at 32 KiB
  python3 run_combined_sweep.py \
      --host 192.168.2.2 --port 8444 \
      --label vanilla_one_container \
      --alpha-list 0 \
      --payload-sizes 32768 \
      --num-requests-per-payload 100 --warmup-per-payload 20 \
      --requests-per-conn 50 \
      --out results/vanilla_one_container_alpha0_step32.csv
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

DEFAULT_ALPHA_VALUES = [0, 25, 50]

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
                           warmup_per_payload: int | None,
                           payload_sizes: list[int] | None = None) -> list[tuple[int, int, int]]:
    default_schedule = {size: (reqs, warmup) for size, reqs, warmup in DEFAULT_PAYLOAD_SCHEDULE}

    if payload_sizes is None:
        selected_payload_sizes = [size for size, _, _ in DEFAULT_PAYLOAD_SCHEDULE]
    else:
        selected_payload_sizes = payload_sizes

    if num_requests_per_payload is None and warmup_per_payload is None and payload_sizes is None:
        return list(DEFAULT_PAYLOAD_SCHEDULE)

    if num_requests_per_payload is None:
        schedule: list[tuple[int, int, int]] = []
        missing_sizes: list[int] = []
        for size in selected_payload_sizes:
            if size not in default_schedule:
                missing_sizes.append(size)
                continue
            reqs, default_warmup = default_schedule[size]
            schedule.append((size, reqs, warmup_per_payload if warmup_per_payload is not None else default_warmup))

        if missing_sizes:
            missing = ", ".join(str(size) for size in missing_sizes)
            raise ValueError(
                f"custom payload sizes [{missing}] require --num-requests-per-payload"
            )
        return schedule

    if warmup_per_payload is None:
        warmup_per_payload = min(20, max(1, num_requests_per_payload // 10))

    return [(size, num_requests_per_payload, warmup_per_payload) for size in selected_payload_sizes]


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


# ─── Connection pool with per-connection request limit ─────────────────────

class ConnPool:
    """Opens a fresh TLS connection every `requests_per_conn` requests."""

    def __init__(self, host: str, port: int, timeout: float,
                 requests_per_conn: int):
        self.host  = host
        self.port  = port
        self.timeout = timeout
        self.rpc   = requests_per_conn  # 0 = unlimited
        self._conn: http.client.HTTPSConnection | None = None
        self._count = 0

    def _open(self) -> None:
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
        self._conn  = _make_conn(self.host, self.port, self.timeout)
        self._count = 0

    def request(self, fn_name: str, payload: bytes) -> dict:
        if self._conn is None:
            self._open()
        if self.rpc > 0 and self._count >= self.rpc:
            self._open()
        try:
            data = do_request(self._conn, fn_name, payload)
            self._count += 1
            return data
        except Exception:
            self._open()
            data = do_request(self._conn, fn_name, payload)
            self._count += 1
            return data

    def close(self) -> None:
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None


# ─── Alpha schedule builder ────────────────────────────────────────────────

def build_schedule(fn_a: str, fn_b: str,
                   n_total: int, alpha: int,
                   rng: random.Random) -> list[str]:
    """
    alpha=0  → strictly alternating A B A B …
    alpha=25 → 25% block to fn_a + 25% block to fn_b + 50% random
    alpha=50 → 50% block to fn_a + 50% block to fn_b (no random)
    alpha=100 → all requests go to fn_a.

    For alpha=100, fn_a is treated as the pinned owner. This preserves the
    existing 0/25/50 semantics while providing a strict same-owner extreme.
    """
    if alpha == 100:
        return [fn_a] * n_total

    alpha = max(0, min(50, alpha))

    if alpha == 0:
        return [fn_a if i % 2 == 0 else fn_b for i in range(n_total)]

    n_burst = round(n_total * alpha / 100)
    n_rand  = max(0, n_total - 2 * n_burst)

    seq = ([fn_a] * n_burst
           + [fn_b] * n_burst
           + [rng.choice([fn_a, fn_b]) for _ in range(n_rand)])
    while len(seq) < n_total:
        seq.append(rng.choice([fn_a, fn_b]))
    return seq[:n_total]


# ─── Core sweep ───────────────────────────────────────────────────────────

def run_sweep(host: str, port: int,
              fn_a: str, fn_b: str,
              alpha_values: list[int],
              payload_schedule: list[tuple[int, int, int]],
              requests_per_conn: int,
              timeout: float,
              label: str,
              out_path: str,
              seed: int = 42) -> None:

    rng = random.Random(seed)

    total_runs = len(alpha_values) * len(payload_schedule)
    run_no = 0

    fieldnames = [
        "label", "alpha", "payload_size",
        "request_seq", "request_no",
        "worker", "target_fn", "switched",
        "delta_cycles", "cntfrq", "delta_ns",
        "requests_per_conn", "top1_rdtsc", "top2_rdtsc",
    ]
    expected = sum(n for _, n, _ in payload_schedule) * len(alpha_values)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    rows_written = 0
    with open(out_path, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        fh.flush()

        for alpha in alpha_values:
            for payload_size, num_requests, warmup in payload_schedule:
                run_no += 1
                print(f"\n[{run_no:2d}/{total_runs}]  alpha={alpha:2d}  "
                      f"payload={payload_size:>10,} B  ({num_requests} req + {warmup} warmup)",
                      flush=True)

                # Pre-allocate a single payload buffer for this run (fixed size)
                payload = os.urandom(payload_size)

                warmup_sched = build_schedule(fn_a, fn_b, warmup, alpha, rng)
                measure_sched = build_schedule(fn_a, fn_b, num_requests, alpha, rng)

                pool = ConnPool(host, port, timeout, requests_per_conn)

                # ── warmup ──────────────────────────────────────────────────
                for fn in warmup_sched:
                    try:
                        pool.request(fn, payload)
                    except Exception as e:
                        print(f"  warmup error (ignored): {e}", file=sys.stderr, flush=True)

                # ── measure ─────────────────────────────────────────────────
                prev_fn = warmup_sched[-1] if warmup_sched else fn_a
                errors = 0
                for seq_i, fn in enumerate(measure_sched):
                    switched = int(fn != prev_fn)

                    try:
                        data = pool.request(fn, payload)
                    except Exception as e:
                        errors += 1
                        print(f"  req {seq_i} error: {e}", file=sys.stderr, flush=True)
                        prev_fn = fn
                        continue

                    cntfrq = data.get("cntfrq", 1) or 1
                    delta_cycles = data.get("delta_cycles", 0)

                    writer.writerow({
                        "label": label,
                        "alpha": alpha,
                        "payload_size": payload_size,
                        "request_seq": seq_i + 1,
                        "request_no": data.get("request_no", seq_i + 1),
                        "worker": data.get("worker", ""),
                        "target_fn": fn,
                        "switched": switched,
                        "delta_cycles": delta_cycles,
                        "cntfrq": cntfrq,
                        "delta_ns": int(delta_cycles * 1_000_000_000 / cntfrq),
                        "requests_per_conn": requests_per_conn,
                        "top1_rdtsc": data.get("top1_rdtsc", 0),
                        "top2_rdtsc": data.get("top2_rdtsc", 0),
                    })
                    rows_written += 1
                    fh.flush()
                    prev_fn = fn

                    if seq_i == 0 or (seq_i + 1) % 5 == 0 or seq_i + 1 == num_requests:
                        print(f"  progress {seq_i + 1}/{num_requests} ok_rows={rows_written}", flush=True)

                pool.close()
                fh.flush()
                ok = num_requests - errors
                print(f"  done — {ok} ok, {errors} errors", flush=True)

    print(f"\nWrote {rows_written}/{expected} rows → {out_path}", flush=True)


def parse_alpha_list(value: str | None) -> list[int]:
    if value is None:
        return list(DEFAULT_ALPHA_VALUES)

    alpha_values: list[int] = []
    for chunk in value.split(","):
        item = chunk.strip()
        if not item:
            continue
        alpha = int(item)
        if not ((0 <= alpha <= 50) or alpha == 100):
            raise ValueError(
                f"alpha {alpha} is out of range [0, 50] or the special value 100"
            )
        alpha_values.append(alpha)

    if not alpha_values:
        raise ValueError("alpha list cannot be empty")
    return alpha_values


def parse_payload_sizes(value: str | None) -> list[int] | None:
    if value is None:
        return None

    payload_sizes: list[int] = []
    seen: set[int] = set()
    for chunk in value.split(","):
        item = chunk.strip()
        if not item:
            continue
        size = int(item)
        if size <= 0:
            raise ValueError("payload sizes must be > 0")
        if size in seen:
            continue
        seen.add(size)
        payload_sizes.append(size)

    if not payload_sizes:
        raise ValueError("payload size list cannot be empty")
    return payload_sizes


# ─── main ─────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Combined sweep: alpha × payload size grid → one CSV",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host",              default="192.168.2.2")
    ap.add_argument("--port",              type=int, default=8444,
                    help="8444=vanilla gateway TLS listener / 9444=proto gateway")
    ap.add_argument("--function-a",        default="bench2-fn-a")
    ap.add_argument("--function-b",        default="bench2-fn-b")
    ap.add_argument("--requests-per-conn", type=int, default=50,
                    help="requests per TLS connection before reconnecting (0=unlimited)")
    ap.add_argument("--timeout",           type=float, default=30.0)
    ap.add_argument("--label",             default="vanilla",
                    help="label written into the CSV (vanilla or proto)")
    ap.add_argument("--seed",              type=int, default=42,
                    help="random seed (same seed → reproducible schedule)")
    ap.add_argument("--alpha-list",        default=None,
                    help="comma-separated alpha values, default 0,25,50; supports special value 100")
    ap.add_argument("--payload-sizes",     default=None,
                    help="comma-separated payload sizes in bytes, e.g. 32768 or 1024,4096,32768")
    ap.add_argument("--num-requests-per-payload", type=int, default=None,
                    help="override the mixed schedule and use the same request count for every payload size")
    ap.add_argument("--warmup-per-payload", type=int, default=None,
                    help="override the warmup count for every payload size")
    ap.add_argument("--out",               required=True,
                    help="output CSV path, e.g. results/combined_vanilla.csv")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    try:
        alpha_values = parse_alpha_list(args.alpha_list)
        payload_sizes = parse_payload_sizes(args.payload_sizes)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.num_requests_per_payload is not None and args.num_requests_per_payload <= 0:
        print("error: --num-requests-per-payload must be > 0", file=sys.stderr)
        return 2
    if args.warmup_per_payload is not None and args.warmup_per_payload < 0:
        print("error: --warmup-per-payload must be >= 0", file=sys.stderr)
        return 2

    payload_schedule = build_payload_schedule(
        num_requests_per_payload=args.num_requests_per_payload,
        warmup_per_payload=args.warmup_per_payload,
        payload_sizes=payload_sizes,
    )

    total_rows = sum(n for _, n, _ in payload_schedule) * len(alpha_values)
    print(f"Payload schedule (size, requests, warmup):")
    for size, nreq, nwarm in payload_schedule:
        print(f"  {size:>10,} B  →  {nreq:4d} req  + {nwarm} warmup")
    print(f"\nAlpha values: {alpha_values}")
    print(f"Total runs: {len(alpha_values) * len(payload_schedule)}  "
          f"({len(alpha_values)} alpha × {len(payload_schedule)} sizes)")
    print(f"Total rows expected: {total_rows:,}")
    print(f"Requests per TLS conn: {args.requests_per_conn or 'unlimited'}")
    print(f"Label: {args.label}")
    print(f"Output: {args.out}")

    run_sweep(
        host=args.host,
        port=args.port,
        fn_a=args.function_a,
        fn_b=args.function_b,
        alpha_values=alpha_values,
        payload_schedule=payload_schedule,
        requests_per_conn=args.requests_per_conn,
        timeout=args.timeout,
        label=args.label,
        out_path=args.out,
        seed=args.seed,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
