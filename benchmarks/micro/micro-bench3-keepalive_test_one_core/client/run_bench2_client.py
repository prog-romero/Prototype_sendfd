#!/usr/bin/env python3
"""
run_bench2_client.py — Benchmark-2 keepalive HTTPS client.

Measures the top1→top2 delta reported by the container function
(delta_cycles / cntfrq → nanoseconds).  The delta is computed server-side
from rdtsc/CNTVCT_EL0 stamps that are globally synchronised across all Pi
cores, so it is unaffected by network jitter.

━━━ Alpha semantics (case2) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

alpha ∈ [0, 50] or alpha = 100.

For 0..50, alpha controls how many requests are sent in consecutive bursts
to the same worker before distributing the rest randomly.

For N total requests with a given alpha:
  n_burst = round(N * alpha / 100)          ← consecutive block to fn-a
  n_burst = round(N * alpha / 100)          ← consecutive block to fn-b
  n_rand  = N - 2 * n_burst                ← randomly to fn-a or fn-b

  alpha=0  → strictly alternating : A B A B A B …
  alpha=10 → 10 % to A | 10 % to B | 80 % random
  alpha=25 → 25 % to A | 25 % to B | 50 % random
    alpha=50 → 50 % to A | 50 % to B | 0 % random   (two big consecutive runs)
    alpha=100 → 100 % to function A only            (pin all requests to one owner)

Use --alpha-sweep (or --alpha-list) to run all alpha values automatically.

━━━ Connection reuse ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

--requests-per-conn N  — number of requests on each TLS connection before
opening a new one.  Default = 0 (all requests on one connection).
Increasing this shows how the keepalive benefit changes with connection length.

━━━ Usage examples ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  # Case 1 — single function
  python3 run_bench2_client.py \\
      --host 192.168.2.2 --port 8444 --mode case1 \\
      --function bench2-fn-a \\
      --num-requests 500 --payload-size 512 --warmup 20 \\
      --requests-per-conn 50 \\
      --out results/vanilla_case1_p512.csv

  # Case 2 — single alpha value
  python3 run_bench2_client.py \\
      --host 192.168.2.2 --port 8444 --mode case2 \\
      --function-a bench2-fn-a --function-b bench2-fn-b \\
      --num-requests 500 --payload-size 512 --alpha 25 --warmup 20 \\
      --requests-per-conn 50 \\
      --out results/vanilla_case2_alpha25_p512.csv

  # Case 2 — sweep all alpha values (writes one CSV per alpha)
  python3 run_bench2_client.py \\
      --host 192.168.2.2 --port 8444 --mode case2 \\
      --function-a bench2-fn-a --function-b bench2-fn-b \\
      --num-requests 500 --payload-size 512 --warmup 20 \\
      --requests-per-conn 50 \\
      --alpha-sweep \\
      --out results/vanilla_case2_p512
"""

import argparse
import csv
import http.client
import json
import os
import random
import ssl
import sys


# ─── Default alpha values ──────────────────────────────────────────────────
# 20 values from 0 to 50 (step 2.5, rounded to one decimal)
DEFAULT_ALPHA_LIST = [0, 2, 5, 7, 10, 12, 15, 17, 20, 22,
                      25, 27, 30, 32, 35, 37, 40, 42, 45, 50]


# ─── SSL / connection helpers ──────────────────────────────────────────────

def _make_ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def _make_conn(host: str, port: int, timeout: float) -> http.client.HTTPSConnection:
    ctx = _make_ssl_ctx()
    return http.client.HTTPSConnection(host, port=port, context=ctx, timeout=timeout)


# ─── Single request ────────────────────────────────────────────────────────

def do_request(conn: http.client.HTTPSConnection,
               fn_name: str,
               payload_bytes: bytes,
               keep_alive: bool = True,
               single_owner_hint: bool = False) -> dict:
    headers = {
        "Content-Type":   "application/octet-stream",
        "Content-Length": str(len(payload_bytes)),
        "Connection":     "keep-alive" if keep_alive else "close",
    }
    if single_owner_hint:
        headers["X-Bench2-Single-Owner"] = "1"
    conn.request("POST", f"/function/{fn_name}", body=payload_bytes, headers=headers)
    resp = conn.getresponse()
    raw  = resp.read()
    return json.loads(raw.decode())


# ─── Connection pool with per-connection request limit ─────────────────────

class ConnPool:
    """Opens a new TLS connection every `requests_per_conn` requests."""

    def __init__(self, host: str, port: int, timeout: float,
                 requests_per_conn: int,
                 single_owner_hint: bool = False):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.rpc  = requests_per_conn   # 0 = unlimited
        self.single_owner_hint = single_owner_hint
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
            data = do_request(self._conn, fn_name, payload,
                              single_owner_hint=self.single_owner_hint)
            self._count += 1
            return data
        except Exception:
            # reconnect once on error
            self._open()
            data = do_request(self._conn, fn_name, payload,
                              single_owner_hint=self.single_owner_hint)
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
    Build a list of n_total function names for the given alpha.

    alpha=0  → strictly alternating A B A B …
    alpha∈(0,50] →
        Block A : round(n_total * alpha/100) consecutive requests to fn_a
        Block B : round(n_total * alpha/100) consecutive requests to fn_b
        Random  : remaining requests, each randomly fn_a or fn_b
    alpha=100 → all requests go to fn_a.

    The special-case alpha=100 is used for the pure single-owner locality
    extreme. For that mode, fn_a is treated as the pinned owner.
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
    # Trim/pad to exactly n_total (rounding may cause ±1)
    while len(seq) < n_total:
        seq.append(rng.choice([fn_a, fn_b]))
    return seq[:n_total]


# ─── Case 1: single function ───────────────────────────────────────────────

def run_case1(host: str, port: int,
              function_name: str,
              num_requests: int,
              payload_size: int,
              warmup: int,
              timeout: float,
              requests_per_conn: int,
              out_path: str) -> None:
    """All requests go to the same function. Controls requests-per-connection."""
    payload = os.urandom(payload_size)
    pool    = ConnPool(host, port, timeout, requests_per_conn,
                       single_owner_hint=True)

    print(f"[case1] warmup {warmup} requests …")
    for _ in range(warmup):
        try:
            pool.request(function_name, payload)
        except Exception as e:
            print(f"  warmup error (ignored): {e}", file=sys.stderr)

    print(f"[case1] measuring {num_requests} requests "
          f"(rpc={requests_per_conn or 'unlimited'}) …")
    rows = []
    for i in range(num_requests):
        try:
            data = pool.request(function_name, payload)
        except Exception as e:
            print(f"  request {i} error: {e}", file=sys.stderr)
            continue

        cntfrq       = data.get("cntfrq", 1) or 1
        delta_cycles = data.get("delta_cycles", 0)
        rows.append({
            "request_no":        data.get("request_no", i + 1),
            "worker":            data.get("worker", ""),
            "delta_cycles":      delta_cycles,
            "cntfrq":            cntfrq,
            "delta_ns":          int(delta_cycles * 1_000_000_000 / cntfrq),
            "payload_size":      payload_size,
            "requests_per_conn": requests_per_conn,
            "top1_rdtsc":        data.get("top1_rdtsc", 0),
            "top2_rdtsc":        data.get("top2_rdtsc", 0),
        })

    pool.close()
    _write_csv(out_path, rows,
               ["request_no", "worker", "delta_cycles", "cntfrq", "delta_ns",
                "payload_size", "requests_per_conn", "top1_rdtsc", "top2_rdtsc"])
    print(f"[case1] wrote {len(rows)} rows → {out_path}")


# ─── Case 2: two functions with alpha locality ─────────────────────────────

def run_case2_alpha(host: str, port: int,
                    fn_a: str, fn_b: str,
                    num_requests: int,
                    payload_size: int,
                    alpha: int,
                    warmup: int,
                    timeout: float,
                    requests_per_conn: int,
                    out_path: str) -> None:
    """
    Run case2 for a single alpha value.

    Schedule:
    alpha=0  → strictly alternating A B A B …
    alpha∈(0,50] →
          [alpha% to fn_a] [alpha% to fn_b] [(100-2*alpha)% randomly to A or B]
    alpha=100 → all requests pinned to fn_a
    """
    rng     = random.Random(42)
    payload = os.urandom(payload_size)

    warmup_sched  = build_schedule(fn_a, fn_b, warmup,       alpha, rng)
    measure_sched = build_schedule(fn_a, fn_b, num_requests, alpha, rng)

    single_owner_hint = len(set(warmup_sched + measure_sched)) == 1
    pool = ConnPool(host, port, timeout, requests_per_conn,
                    single_owner_hint=single_owner_hint)

    print(f"[case2] alpha={alpha:3d} — warmup {warmup} requests …")
    for fn in warmup_sched:
        try:
            pool.request(fn, payload)
        except Exception as e:
            print(f"  warmup error (ignored): {e}", file=sys.stderr)

    prev_fn = warmup_sched[-1] if warmup_sched else fn_a
    print(f"[case2] alpha={alpha:3d} — measuring {num_requests} requests "
          f"(rpc={requests_per_conn or 'unlimited'}) …")
    rows = []
    for i, fn in enumerate(measure_sched):
        switched = int(fn != prev_fn)
        try:
            data = pool.request(fn, payload)
        except Exception as e:
            print(f"  request {i} error: {e}", file=sys.stderr)
            prev_fn = fn
            continue

        cntfrq       = data.get("cntfrq", 1) or 1
        delta_cycles = data.get("delta_cycles", 0)
        rows.append({
            "request_no":        data.get("request_no", i + 1),
            "worker":            data.get("worker", ""),
            "target_fn":         fn,
            "switched":          switched,
            "alpha":             alpha,
            "delta_cycles":      delta_cycles,
            "cntfrq":            cntfrq,
            "delta_ns":          int(delta_cycles * 1_000_000_000 / cntfrq),
            "payload_size":      payload_size,
            "requests_per_conn": requests_per_conn,
            "top1_rdtsc":        data.get("top1_rdtsc", 0),
            "top2_rdtsc":        data.get("top2_rdtsc", 0),
        })
        prev_fn = fn

    pool.close()
    _write_csv(out_path, rows,
               ["request_no", "worker", "target_fn", "switched", "alpha",
                "delta_cycles", "cntfrq", "delta_ns", "payload_size",
                "requests_per_conn", "top1_rdtsc", "top2_rdtsc"])
    print(f"[case2] alpha={alpha:3d} wrote {len(rows)} rows → {out_path}")


def run_case2_sweep(host: str, port: int,
                    fn_a: str, fn_b: str,
                    num_requests: int,
                    payload_size: int,
                    alpha_list: list[int],
                    warmup: int,
                    timeout: float,
                    requests_per_conn: int,
                    out_prefix: str) -> None:
    """Run case2 for every alpha in alpha_list, writing one CSV per alpha."""
    for alpha in alpha_list:
        out_path = f"{out_prefix}_alpha{alpha:03d}.csv"
        run_case2_alpha(
            host=host, port=port,
            fn_a=fn_a, fn_b=fn_b,
            num_requests=num_requests,
            payload_size=payload_size,
            alpha=alpha,
            warmup=warmup,
            timeout=timeout,
            requests_per_conn=requests_per_conn,
            out_path=out_path,
        )


# ─── CSV helper ────────────────────────────────────────────────────────────

def _write_csv(path: str, rows: list, fieldnames: list) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


# ─── main ──────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Benchmark-2 keepalive HTTPS client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host",               default="192.168.2.2")
    ap.add_argument("--port",               type=int, default=8444)
    ap.add_argument("--mode",               choices=["case1", "case2"], required=True)
    ap.add_argument("--function",           default="bench2-fn-a",
                    help="function name (case1)")
    ap.add_argument("--function-a",         default="bench2-fn-a",
                    help="first function (case2)")
    ap.add_argument("--function-b",         default="bench2-fn-b",
                    help="second function (case2)")
    ap.add_argument("--num-requests",       type=int, default=500)
    ap.add_argument("--payload-size",       type=int, default=512,
                    help="request body size in bytes")
    ap.add_argument("--warmup",             type=int, default=20)
    ap.add_argument("--timeout",            type=float, default=15.0)
    ap.add_argument("--requests-per-conn",  type=int, default=0,
                    help="requests per TLS connection (0 = unlimited / one for all)")

    # alpha options (case2)
    ap.add_argument("--alpha",      type=int, default=None,
                    help="single alpha value 0-50, or 100 for all requests on function-a")
    ap.add_argument("--alpha-sweep", action="store_true",
                    help="run all default alpha values automatically (case2)")
    ap.add_argument("--alpha-list", default=None,
                    help="comma-separated list of alpha values, e.g. 0,10,25,50 or 0,100")

    ap.add_argument("--out", required=True,
                    help="output CSV path (case1/case2 single alpha) "
                         "or output prefix (case2 sweep)")
    args = ap.parse_args()

    if args.mode == "case1":
        run_case1(
            host=args.host, port=args.port,
            function_name=args.function,
            num_requests=args.num_requests,
            payload_size=args.payload_size,
            warmup=args.warmup,
            timeout=args.timeout,
            requests_per_conn=args.requests_per_conn,
            out_path=args.out,
        )

    else:  # case2
        # Resolve alpha list
        if args.alpha_list:
            alpha_list = [int(x.strip()) for x in args.alpha_list.split(",")]
        elif args.alpha_sweep or args.alpha is None:
            alpha_list = DEFAULT_ALPHA_LIST
        else:
            alpha_list = [args.alpha]

        # Validate
        for a in alpha_list:
            if not ((0 <= a <= 50) or a == 100):
                print(f"error: alpha {a} is out of range [0, 50] or the special value 100", file=sys.stderr)
                return 2

        if len(alpha_list) == 1 and not args.alpha_sweep:
            # Single alpha → write directly to --out
            run_case2_alpha(
                host=args.host, port=args.port,
                fn_a=args.function_a, fn_b=args.function_b,
                num_requests=args.num_requests,
                payload_size=args.payload_size,
                alpha=alpha_list[0],
                warmup=args.warmup,
                timeout=args.timeout,
                requests_per_conn=args.requests_per_conn,
                out_path=args.out,
            )
        else:
            # Sweep → --out is treated as a filename prefix
            run_case2_sweep(
                host=args.host, port=args.port,
                fn_a=args.function_a, fn_b=args.function_b,
                num_requests=args.num_requests,
                payload_size=args.payload_size,
                alpha_list=alpha_list,
                warmup=args.warmup,
                timeout=args.timeout,
                requests_per_conn=args.requests_per_conn,
                out_prefix=args.out,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
