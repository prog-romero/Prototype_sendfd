#!/usr/bin/env python3
"""
Latence de base — UNE nouvelle connexion par requête, sans keep-alive — pour le
macro-bench BeFaaS IoT (point d'entrée objectrecognition, requête = image).

Même méthodologie et même format CSV que ../../evaluation_base/run_eval.py, mais
la requête est un POST multipart d'une IMAGE (pas un payload octet-stream), et
l'axe "taille" est la taille de l'image uploadée.

wrk2 est lancé en -c1 -t1 -R<rate> avec un Lua qui ajoute "Connection: close" :
chaque requête ouvre donc une connexion TCP+TLS NEUVE, et la latence mesurée est
EXACTEMENT ce que mesure `curl -w %{time_total}` pour une requête :

    TCP SYN/ACK + handshake TLS + upload image + traitement serveur
    (toute la chaîne objectrecognition: jimp + 3 hops migrés + Redis) + réponse.

Pour chaque taille d'image, on collecte avg/stdev/max/p50/p75/p90/p99 et on
écrit une ligne CSV.

Usage
-----
  # Prototype, HTTPS :
  python3 run_base_latence.py --mode proto --scheme https --host 192.168.2.2 \\
      --sizes 8,18,28,38,48,58 --requests 50 \\
      --output results/proto_https.csv

  # Vanilla, HTTPS :
  python3 run_base_latence.py --mode vanilla --scheme https --host 192.168.2.2 \\
      --sizes 8,18,28,38,48,58 --requests 50 \\
      --output results/vanilla_https.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

# ── parsers wrk2 (identiques à evaluation_base/run_eval.py) ───────────────────

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


def _to_ms(value: float, unit: str) -> float:
    if unit == "us":
        return value / 1000.0
    if unit == "ms":
        return value
    if unit == "s":
        return value * 1000.0
    return value


def _parse_float_or_zero(raw: str) -> float:
    low = raw.strip().lower()
    if low in {"nan", "-nan", "+nan"}:
        return 0.0
    return float(raw)


def _parse_wrk2_output(text: str) -> dict:
    m = _LATENCY_STATS_RE.search(text)
    if m:
        avg_ms = _to_ms(_parse_float_or_zero(m.group("avg")), m.group("avg_u"))
        stdev_ms = _to_ms(_parse_float_or_zero(m.group("stdev")), m.group("stdev_u"))
        max_ms = _to_ms(_parse_float_or_zero(m.group("max")), m.group("max_u"))
    else:
        avg_ms = stdev_ms = max_ms = 0.0
        print("  [WARN] ligne Latency wrk2 introuvable — sortie brute :")
        for line in text.splitlines():
            print(f"    | {line}")

    pcts: dict[str, float] = {}
    for pm in _PERCENTILE_RE.finditer(text):
        key = f"p{float(pm.group('pct')):g}_ms"
        pcts[key] = _to_ms(float(pm.group("val")), pm.group("unit"))

    req_m = _REQUESTS_IN_RE.search(text)
    err_m = _ERRORS_RE.search(text)
    sock_m = _SOCKET_ERRORS_RE.search(text)

    return {
        "avg_ms": round(avg_ms, 3),
        "stdev_ms": round(stdev_ms, 3),
        "max_ms": round(max_ms, 3),
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


def _find_wrk2() -> str:
    env = os.environ.get("WRK2")
    if env:
        e = os.path.expanduser(env)
        if os.path.isfile(e) and os.access(e, os.X_OK):
            return e
    for candidate in (os.path.expanduser("~/wrk2/wrk"), os.path.expanduser("~/wrk2/wrk2")):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return "wrk2"


def _run_wrk2(wrk2, lua, url, fn_name, image_path, n_requests, rate, timeout_s):
    duration_s = max(n_requests // rate + 5, 10)
    env = os.environ.copy()
    env["WRK_IMAGE_PATH"] = image_path
    env["WRK_FUNCTION"] = fn_name
    cmd = [
        wrk2, "-t1", "-c1", f"-d{duration_s}s", f"-R{rate}",
        "--timeout", f"{timeout_s}s", "--latency", "-s", lua, url,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=duration_s + timeout_s + 30)
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 introuvable — installez-le dans ~/wrk2/wrk ou WRK2=..."
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main() -> None:
    p = argparse.ArgumentParser(
        description="Latence de base (1 connexion TCP+TLS neuve par requête) — image BeFaaS."
    )
    p.add_argument("--mode", choices=["proto", "vanilla"], required=True,
                   help="proto = chemin sendfd ; vanilla = proxy OpenFaaS standard (label CSV)")
    p.add_argument("--scheme", choices=["https", "http"], default="https",
                   help="https (8443, défaut) ou http (8080)")
    p.add_argument("--host", default="192.168.2.2", help="IP du Pi")
    p.add_argument("--port", type=int, default=None, help="Override port (def 8443/8080)")
    p.add_argument("--sizes", default="2,4,8,16,32,64,128,256,512,1024",
                   help="Tailles d'image en KB, séparées par des virgules (def 2,4,8,16,...,1024)")
    p.add_argument("--images-dir", default="images",
                   help="Dossier des images img-<KB>kb.jpg (relatif au script)")
    p.add_argument("--requests", type=int, default=50, dest="n_requests",
                   help="Nb de requêtes visé par taille (def 50)")
    p.add_argument("--rate", type=int, default=2,
                   help="Débit constant wrk2 en req/s (def 2). -c1 + Connection:close "
                        "=> une session TLS neuve par requête. Durée = n_requests/rate + 5s.")
    p.add_argument("--timeout-s", type=int, default=30, help="Timeout requête (s)")
    p.add_argument("--function", default="objectrecognition", help="Fonction d'entrée")
    p.add_argument("--output", default="results.csv", help="Chemin du CSV de sortie")
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    url = f"{args.scheme}://{args.host}:{port}/function/{args.function}"

    wrk2 = _find_wrk2()
    script_dir = Path(__file__).resolve().parent
    lua = str(script_dir / "client" / "post_image_nokeep.lua")
    images_dir = Path(args.images_dir)
    if not images_dir.is_absolute():
        images_dir = script_dir / images_dir
    sizes = [int(s.strip()) for s in args.sizes.split(",")]

    print(f"mode      : {args.mode}")
    print(f"scheme    : {args.scheme}  (port {port})")
    print(f"url       : {url}")
    print(f"function  : {args.function}")
    print(f"sizes KB  : {sizes}")
    print(f"requests  : ~{args.n_requests} par taille (rate={args.rate} req/s, -c1, Connection:close)")
    print(f"wrk2      : {wrk2}")
    print(f"images    : {images_dir}")
    print(f"output    : {args.output}")
    print()

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    rows = []

    for size_kb in sizes:
        img = images_dir / f"img-{size_kb}kb.jpg"
        if not img.is_file():
            print(f"  [{size_kb:>4} KB]  IMAGE INTROUVABLE: {img} — passée")
            continue
        actual_bytes = img.stat().st_size
        duration_s = max(args.n_requests // args.rate + 5, 10)
        print(f"  [{size_kb:>4} KB]  ({actual_bytes} o)  wrk2 -c1 -t1 -R{args.rate} -d{duration_s}s ...",
              end="", flush=True)

        rc, output = _run_wrk2(wrk2, lua, url, args.function, str(img),
                               args.n_requests, args.rate, args.timeout_s)
        parsed = _parse_wrk2_output(output)

        server_errors = (parsed["errors_non2xx"] + parsed["socket_connect_errors"]
                         + parsed["socket_read_errors"] + parsed["socket_write_errors"])
        warn = f"  ⚠ server_errors={server_errors}" if server_errors else ""
        print(f"  avg={parsed['avg_ms']:7.2f}ms  stdev={parsed['stdev_ms']:6.2f}ms"
              f"  p99={parsed['p99_ms']:7.2f}ms  n={parsed['total_requests']}{warn}")
        if server_errors:
            print("    !! erreurs serveur — vérifier: sudo ctr -n openfaas-fn task ls")

        rows.append({
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "mode": args.mode,
            "scheme": args.scheme,
            "function": args.function,
            "size_kb": size_kb,
            "image_bytes": actual_bytes,
            "n_requests_target": args.n_requests,
            "rate_rps": args.rate,
            "total_requests": parsed["total_requests"],
            "avg_ms": parsed["avg_ms"],
            "stdev_ms": parsed["stdev_ms"],
            "max_ms": parsed["max_ms"],
            "p50_ms": parsed["p50_ms"],
            "p75_ms": parsed["p75_ms"],
            "p90_ms": parsed["p90_ms"],
            "p99_ms": parsed["p99_ms"],
            "errors_non2xx": parsed["errors_non2xx"],
            "socket_connect_errors": parsed["socket_connect_errors"],
            "socket_read_errors": parsed["socket_read_errors"],
            "socket_write_errors": parsed["socket_write_errors"],
            "socket_timeout_errors": parsed["socket_timeout_errors"],
            "exit_code": rc,
        })

    if not rows:
        print("Aucune ligne produite (images manquantes ?).")
        return
    with open(args.output, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nSauvegardé → {args.output}")


if __name__ == "__main__":
    main()
