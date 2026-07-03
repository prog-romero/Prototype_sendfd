#!/usr/bin/env python3
"""
run_eval.py — LATENCE DE BASE d'une fonction SeBS (vanilla vs proto).

Adapté de micro-bench3-keepalive-https-integration/evaluation_base/run_eval.py,
mais au lieu de balayer des tailles de payload, on POSTe l'event JSON SeBS de la
fonction. Une NOUVELLE connexion TCP+TLS par requête (wrk2 -c1 -t1 +
"Connection: close") -> latence bout-en-bout à VIDE (aucune charge concurrente).

Ce qui est mesuré
-----------------
Prototype (--mode proto):
    client → gateway (peek TLS + sendfd) → provider.sock → watchdog → fonction
    → la fonction répond DIRECTEMENT au client (pas de retour par faasd).
Vanilla (--mode vanilla):
    client → gateway → faasd-provider → watchdog → fonction → watchdog
    → faasd-provider → gateway → client (chaîne proxy OpenFaaS standard).

Pour chaque fonction : ~--requests requêtes à --rate req/s, on collecte la
distribution de latence (avg/p50/p75/p90/p99 côté client) et, via le hook Lua,
le server_ms (results_time du wrapper) -> overhead = client - server.

Exemple
-------
  python3 run_eval.py --mode proto --scheme https --host 192.168.2.2 \\
      --function dynamic-html --input ../../inputs/dynamic-html.json \\
      --requests 50 --rate 2 --out results/dynamic-html/base_proto_https.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

# ── parsers wrk2 ─────────────────────────────────────────────────────────────
_LATENCY_STATS_RE = re.compile(
    r"Latency\s+"
    r"(?P<avg>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<avg_u>us|ms|s)\s+"
    r"(?P<stdev>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<stdev_u>us|ms|s)\s+"
    r"(?P<max>-?(?:nan|[0-9]+\.?[0-9]*))\s*(?P<max_u>us|ms|s)",
    re.IGNORECASE,
)
_PERCENTILE_RE = re.compile(
    r"(?P<pct>[0-9]+(?:\.[0-9]+)?)%\s+(?P<val>[0-9]+\.?[0-9]*)\s*(?P<unit>us|ms|s)"
)
_REQUESTS_IN_RE = re.compile(r"(?P<reqs>[0-9,]+)\s+requests in")
_ERRORS_RE = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")
_SOCKET_ERRORS_RE = re.compile(
    r"Socket errors:\s*connect\s+(?P<connect>[0-9]+),\s*read\s+(?P<read>[0-9]+),"
    r"\s*write\s+(?P<write>[0-9]+),\s*timeout\s+(?P<timeout>[0-9]+)"
)


def _to_ms(v, u):
    return v / 1000.0 if u == "us" else (v * 1000.0 if u == "s" else v)


def _f0(raw):
    return 0.0 if raw.strip().lower() in {"nan", "-nan", "+nan"} else float(raw)


def _parse_wrk2(text: str) -> dict:
    m = _LATENCY_STATS_RE.search(text)
    if m:
        avg_ms = _to_ms(_f0(m.group("avg")), m.group("avg_u"))
        stdev_ms = _to_ms(_f0(m.group("stdev")), m.group("stdev_u"))
        max_ms = _to_ms(_f0(m.group("max")), m.group("max_u"))
    else:
        avg_ms = stdev_ms = max_ms = 0.0
    pcts = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pcts[f"p{float(pm.group('pct')):g}_ms"] = _to_ms(float(pm.group("val")), pm.group("unit"))
    req_m = _REQUESTS_IN_RE.search(text)
    err_m = _ERRORS_RE.search(text)
    sock_m = _SOCKET_ERRORS_RE.search(text)
    return {
        "avg_ms": round(avg_ms, 3), "stdev_ms": round(stdev_ms, 3), "max_ms": round(max_ms, 3),
        "p50_ms": round(pcts.get("p50_ms", 0.0), 3),
        "p75_ms": round(pcts.get("p75_ms", 0.0), 3),
        "p90_ms": round(pcts.get("p90_ms", 0.0), 3),
        "p99_ms": round(pcts.get("p99_ms", 0.0), 3),
        "total_requests": int(req_m.group("reqs").replace(",", "")) if req_m else 0,
        "errors_non2xx": int(err_m.group("n")) if err_m else 0,
        "socket_connect_errors": int(sock_m.group("connect")) if sock_m else 0,
        "socket_read_errors": int(sock_m.group("read")) if sock_m else 0,
        "socket_write_errors": int(sock_m.group("write")) if sock_m else 0,
        "socket_timeout_errors": int(sock_m.group("timeout")) if sock_m else 0,
    }


def _pctl(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[idx]


def _read_server_ms(perf_file):
    vals = []
    try:
        with open(perf_file) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        vals.append(float(line) / 1000.0)   # µs -> ms
                    except ValueError:
                        pass
    except FileNotFoundError:
        pass
    return vals


def _find_wrk2() -> str:
    env = os.environ.get("WRK2")
    if env:
        e = os.path.expanduser(env)
        if os.path.isfile(e) and os.access(e, os.X_OK):
            return e
    for c in (os.path.expanduser("~/wrk2/wrk"), os.path.expanduser("~/wrk2/wrk2")):
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return "wrk2"


def _run_wrk2(wrk2, lua, url, req_path, body_file, duration_s, rate, timeout_s, perf_file):
    env = os.environ.copy()
    env["WRK_BODY_FILE"] = body_file
    env["WRK_PATH"] = req_path
    env["WRK_PERF_FILE"] = perf_file
    cmd = [wrk2, "-t1", "-c1", f"-d{duration_s}s", f"-R{rate}",
           "--timeout", f"{timeout_s}s", "--latency", "-s", lua, url]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=duration_s + timeout_s + 30)
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 introuvable"
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main():
    p = argparse.ArgumentParser(description="Latence de base d'une fonction SeBS (connexion neuve par requête).")
    p.add_argument("--mode", choices=["proto", "vanilla"], required=True)
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--port", type=int, default=None, help="override port (def 8443/8080)")
    p.add_argument("--function", required=True, help="nom de la fonction (route)")
    p.add_argument("--input", required=True, help="fichier JSON de l'event à POSTer")
    p.add_argument("--requests", type=int, default=50, help="nb de requêtes visé")
    p.add_argument("--rate", type=int, default=2, help="débit constant req/s (-c1)")
    p.add_argument("--timeout-s", type=int, default=30)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    req_path = f"/function/{args.function}"
    url = f"{args.scheme}://{args.host}:{port}{req_path}"
    body_file = str(Path(args.input).resolve())
    if not Path(body_file).is_file():
        raise SystemExit(f"ERREUR: event introuvable: {body_file}")

    wrk2 = _find_wrk2()
    lua = str(Path(__file__).resolve().parent / "client" / "post_json_nokeep.lua")
    duration_s = max(args.requests // max(args.rate, 1) + 5, 10)
    perf_file = str(Path(args.out).resolve().parent / f".perf_base_{args.function}.tmp")
    try:
        os.remove(perf_file)
    except FileNotFoundError:
        pass

    print(f"=== latence de base [{args.mode}/{args.scheme}] {args.function} ===")
    print(f"url : {url}")
    print(f"~{args.requests} requêtes @ {args.rate} rps (-c1, Connection: close), durée {duration_s}s\n")

    rc, out = _run_wrk2(wrk2, lua, url, req_path, body_file, duration_s, args.rate,
                        args.timeout_s, perf_file)
    d = _parse_wrk2(out)

    server_vals = sorted(_read_server_ms(perf_file))
    if server_vals:
        srv_avg = round(sum(server_vals) / len(server_vals), 3)
        srv_p50 = round(_pctl(server_vals, 0.50), 3)
        srv_p99 = round(_pctl(server_vals, 0.99), 3)
    else:
        srv_avg = srv_p50 = srv_p99 = 0.0
    try:
        os.remove(perf_file)
    except FileNotFoundError:
        pass

    total_errors = (d["errors_non2xx"] + d["socket_connect_errors"]
                    + d["socket_read_errors"] + d["socket_write_errors"]
                    + d["socket_timeout_errors"])
    print(f"  client avg={d['avg_ms']:.2f}ms p50={d['p50_ms']:.2f} p99={d['p99_ms']:.2f}")
    print(f"  server avg={srv_avg:.2f}ms p50={srv_p50:.2f} p99={srv_p99:.2f}")
    print(f"  overhead avg={d['avg_ms']-srv_avg:.2f}ms   n={d['total_requests']} err={total_errors}")

    row = {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "mode": args.mode, "scheme": args.scheme, "function": args.function,
        "n_requests_target": args.requests, "rate_rps": args.rate,
        "total_requests": d["total_requests"],
        "client_ms_avg": d["avg_ms"], "client_ms_stdev": d["stdev_ms"], "client_ms_max": d["max_ms"],
        "client_ms_p50": d["p50_ms"], "client_ms_p75": d["p75_ms"],
        "client_ms_p90": d["p90_ms"], "client_ms_p99": d["p99_ms"],
        "server_ms_avg": srv_avg, "server_ms_p50": srv_p50, "server_ms_p99": srv_p99,
        "overhead_ms_avg": round(d["avg_ms"] - srv_avg, 3),
        "overhead_ms_p50": round(d["p50_ms"] - srv_p50, 3),
        "overhead_ms_p99": round(d["p99_ms"] - srv_p99, 3),
        "server_samples": len(server_vals),
        "errors_non2xx": d["errors_non2xx"],
        "socket_connect_errors": d["socket_connect_errors"],
        "socket_read_errors": d["socket_read_errors"],
        "socket_write_errors": d["socket_write_errors"],
        "socket_timeout_errors": d["socket_timeout_errors"],
        "exit_code": rc,
    }
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(row.keys()))
        w.writeheader()
        w.writerow(row)
    print(f"\n[ok] écrit → {args.out}")


if __name__ == "__main__":
    main()
