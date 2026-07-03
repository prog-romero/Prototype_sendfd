#!/usr/bin/env python3
"""
run_perfcost.py — décomposition par invocation, façon perf-cost de SeBS, adaptée
à faasd. Envoie des requêtes SÉQUENTIELLES (1 à la fois) et lit le JSON du
wrapper SeBS renvoyé par la fonction pour séparer :

  client_ms   : latence end-to-end mesurée ICI (côté client)
  server_ms   : results_time du wrapper SeBS (temps DANS la fonction, µs->ms)
  overhead_ms : client_ms - server_ms = coût du CHEMIN DE DONNÉES
                (réseau + watchdog + proxy/migration) — LA métrique qui isole
                le gain Vanilla vs Prototype.
  compute_ms  : measurement.compute_time du métier (resize/zip/pagerank), si présent
  is_cold     : True au tout premier appel d'un worker (cold start)

Sortie : un CSV (1 ligne/invocation) + un résumé (moyennes/percentiles).

Exemple :
  python3 run_perfcost.py --mode proto --scheme https --host 192.168.2.2 \\
     --function thumbnailer --input ../inputs/thumbnailer.json \\
     --requests 100 --warmup 5 --out results/perfcost_proto_https_thumbnailer.csv
"""
from __future__ import annotations

import argparse
import csv
import json
import ssl
import sys
import time
import urllib.request
from pathlib import Path


def _pct(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[idx]


def invoke(url, body_bytes, ctx, timeout):
    req = urllib.request.Request(url, data=body_bytes, method="POST",
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
        raw = resp.read()
        code = resp.getcode()
    t1 = time.perf_counter()
    client_ms = (t1 - t0) * 1000.0
    parsed = None
    try:
        parsed = json.loads(raw.decode("utf-8"))
    except Exception:
        pass
    return code, client_ms, parsed


def main():
    p = argparse.ArgumentParser(description="perf-cost SeBS (client vs serveur vs overhead) sur faasd.")
    p.add_argument("--mode", choices=["proto", "vanilla"], required=True)
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--function", required=True)
    p.add_argument("--input", required=True, help="fichier JSON de l'event")
    p.add_argument("--requests", type=int, default=100, help="invocations mesurées")
    p.add_argument("--warmup", type=int, default=5, help="invocations de chauffe (non comptées)")
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    url = f"{args.scheme}://{args.host}:{port}/function/{args.function}"
    body_bytes = Path(args.input).read_bytes()

    # cert auto-signé du gateway -> contexte non vérifié pour https
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    print(f"=== perf-cost [{args.mode}/{args.scheme}] {args.function} ===")
    print(f"url      : {url}")
    print(f"requests : {args.requests} (warmup {args.warmup})\n")

    rows = []
    for i in range(args.warmup + args.requests):
        try:
            code, client_ms, parsed = invoke(url, body_bytes, ctx, args.timeout)
        except Exception as e:  # noqa: BLE001
            print(f"  [{i}] ERREUR: {e}", file=sys.stderr)
            continue
        if i < args.warmup:
            continue
        server_ms = 0.0
        compute_ms = 0.0
        is_cold = ""
        if isinstance(parsed, dict):
            server_ms = float(parsed.get("results_time", 0.0)) / 1000.0
            is_cold = parsed.get("is_cold", "")
            res = parsed.get("result", {})
            if isinstance(res, dict) and isinstance(res.get("measurement"), dict):
                compute_ms = float(res["measurement"].get("compute_time", 0.0)) / 1000.0
        overhead_ms = client_ms - server_ms
        rows.append({
            "mode": args.mode, "scheme": args.scheme, "function": args.function,
            "i": i - args.warmup, "http_code": code,
            "client_ms": round(client_ms, 3),
            "server_ms": round(server_ms, 3),
            "overhead_ms": round(overhead_ms, 3),
            "compute_ms": round(compute_ms, 3),
            "is_cold": is_cold,
        })

    if not rows:
        print("Aucune invocation réussie.", file=sys.stderr)
        sys.exit(1)

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # résumé
    def col(name):
        return sorted(r[name] for r in rows)
    cli, srv, ovh = col("client_ms"), col("server_ms"), col("overhead_ms")
    mean = lambda xs: sum(xs) / len(xs)
    print(f"[ok] {len(rows)} invocations -> {args.out}\n")
    print(f"  client_ms   : moy={mean(cli):8.2f}  p50={_pct(cli,0.5):8.2f}  p99={_pct(cli,0.99):8.2f}")
    print(f"  server_ms   : moy={mean(srv):8.2f}  p50={_pct(srv,0.5):8.2f}  p99={_pct(srv,0.99):8.2f}")
    print(f"  overhead_ms : moy={mean(ovh):8.2f}  p50={_pct(ovh,0.5):8.2f}  p99={_pct(ovh,0.99):8.2f}")
    print("  (overhead = client - serveur = coût réseau + watchdog + proxy/migration)")


if __name__ == "__main__":
    main()
