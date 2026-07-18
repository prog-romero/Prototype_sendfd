#!/usr/bin/env python3
"""
microbench_migration.py — mesure les coûts de la migration de connexion.

Envoie N requêtes en SÉQUENTIEL (une à la fois), SANS keep-alive (nouvelle
connexion TCP+TLS à chaque requête, donc une migration fraîche par requête),
depuis UN SEUL client. Puis récupère les logs [MICROBENCH] émis sur le Pi
(gateway + faasd-provider + watchdog de la fonction, tous dans journald) et les
agrège dans un CSV : un coût par requête.

Coûts mesurés (voir l'instrumentation dans le code du prototype) :
  - serialize_ns   : sérialisation TLS (export d'état)         -> gateway
  - deserialize_ns : désérialisation TLS (restore/tls_import)  -> watchdog
  - sendfd_ns      : recvfd(watchdog) - sendfd(provider)        -> provider->watchdog
  - migration_ns   : top2 - top1 (bout-en-bout gateway->container)

Comme les requêtes sont séquentielles et sans keep-alive, chaque requête produit
EXACTEMENT une occurrence de chaque log, dans l'ordre -> on apparie par index.

Pré-requis :
  - graph-pagerank déployée (mode proto) et gateway/faasd/watchdog REBUILDÉS
    avec l'instrumentation [MICROBENCH].
  - accès SSH au Pi ; journald lisible (sinon préfixer --journal-cmd de `sudo`).
  - AUCUN autre trafic vers les fonctions pendant la mesure (sinon les logs
    gateway/watchdog, qui ne portent pas le nom de fonction, seraient mélangés).

Exemple :
  python3 microbench_migration.py --host 192.168.2.2 --pi-ssh romero@192.168.2.2 \\
     --function graph-pagerank --count 100 --out results/microbench_graph.csv
"""
from __future__ import annotations

import argparse
import csv
import http.client
import json
import re
import ssl
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_PAYLOADS = {
    "graph-pagerank": {"size": 510, "seed": 42},
    "dynamic-html": {"username": "testname", "random_len": 1000},
}

# Regex d'extraction des logs [MICROBENCH] / [MIGRATE-VERIFY] (préfixe éventuel ignoré).
RE_SER = re.compile(r"tls_serialize_ns=(\d+)")
RE_DES = re.compile(r"tls_deserialize_ns=(\d+)")
RE_MIG = re.compile(r"migration_ns=(\d+)")
RE_SND = re.compile(r"provider_sendfd_ts=(\d+).*?kind=(\w+)")
RE_RCV = re.compile(r"watchdog_recvfd_ts=(\d+)")
RE_VER = re.compile(r"MIGRATE-VERIFY.*?kind=(\w+)")


def _pctl(xs, q):
    if not xs:
        return 0.0
    s = sorted(xs)
    idx = min(len(s) - 1, int(round(q * (len(s) - 1))))
    return s[idx]


# ── envoi séquentiel, SANS keep-alive (une connexion neuve par requête) ───────
def send_requests(host, port, scheme, path, body, count, timeout):
    if scheme == "https":
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    statuses = []
    for i in range(count):
        # Nouvelle connexion à CHAQUE requête -> nouveau handshake TLS -> migration fraîche.
        if scheme == "https":
            conn = http.client.HTTPSConnection(host, port, timeout=timeout, context=ctx)
        else:
            conn = http.client.HTTPConnection(host, port, timeout=timeout)
        try:
            conn.request("POST", path, body=body,
                         headers={"Content-Type": "application/json", "Connection": "close"})
            resp = conn.getresponse()
            _ = resp.read()
            statuses.append(resp.status)
        except Exception as exc:  # noqa: BLE001
            statuses.append(-1)
            print(f"  req #{i + 1}: ERREUR {exc}", file=sys.stderr)
        finally:
            conn.close()
    return statuses


# ── récupération des logs du Pi (journald) ────────────────────────────────────
def pi_epoch(pi_ssh):
    out = subprocess.run(["ssh", pi_ssh, "date +%s"], capture_output=True, text=True, timeout=20)
    return int(out.stdout.strip())


def fetch_logs(pi_ssh, since_epoch, journal_cmd):
    remote = journal_cmd.format(since=since_epoch)
    out = subprocess.run(["ssh", pi_ssh, remote], capture_output=True, text=True, timeout=60)
    return out.stdout


def parse_logs(text):
    """Retourne, DANS L'ORDRE d'apparition, une liste par métrique."""
    ser, des, mig, rcv = [], [], [], []
    snd, snd_kind, ver_kind = [], [], []
    for line in text.splitlines():
        m = RE_SER.search(line)
        if m: ser.append(int(m.group(1)))
        m = RE_DES.search(line)
        if m: des.append(int(m.group(1)))
        m = RE_MIG.search(line)
        if m: mig.append(int(m.group(1)))
        m = RE_RCV.search(line)
        if m: rcv.append(int(m.group(1)))
        m = RE_SND.search(line)
        if m:
            snd.append(int(m.group(1))); snd_kind.append(m.group(2))
        m = RE_VER.search(line)
        if m: ver_kind.append(m.group(1))
    return {"serialize": ser, "deserialize": des, "migration": mig,
            "recv": rcv, "send": snd, "send_kind": snd_kind, "verify_kind": ver_kind}


def main():
    p = argparse.ArgumentParser(description="Micro-bench des coûts de migration (séquentiel, sans keep-alive).")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--port", type=int, default=None, help="override (def 8443/8080)")
    p.add_argument("--function", default="graph-pagerank")
    p.add_argument("--input", default=None, help="fichier JSON à POSTer (def: payload intégré)")
    p.add_argument("--count", type=int, required=True, help="nombre de requêtes à envoyer")
    p.add_argument("--pi-ssh", default="romero@192.168.2.2")
    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument("--settle", type=float, default=3.0, help="attente (s) avant de lire les logs")
    p.add_argument("--journal-cmd",
                   default="journalctl --since @{since} --no-pager -o cat | grep -E 'MICROBENCH|MIGRATE-VERIFY'",
                   help="commande distante de récupération des logs (préfixer de sudo si besoin)")
    p.add_argument("--out", required=True)
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    path = f"/function/{args.function}"
    if args.input:
        body = Path(args.input).read_bytes()
    else:
        body = json.dumps(DEFAULT_PAYLOADS.get(args.function, {})).encode()

    print(f"=== micro-bench migration : {args.scheme}://{args.host}:{port}{path} ===")
    print(f"    {args.count} requêtes séquentielles, SANS keep-alive (1 connexion / requête)\n")

    since = pi_epoch(args.pi_ssh)          # borne temps pour filtrer le journal
    t0 = time.time()
    statuses = send_requests(args.host, port, args.scheme, path, body, args.count, args.timeout)
    dur = time.time() - t0
    ok = sum(1 for s in statuses if s == 200)
    print(f"  envoi terminé : {ok}/{args.count} en 200 ({dur:.1f}s)")

    time.sleep(args.settle)                # laisse journald flusher
    print("  récupération des logs du Pi…")
    logs = fetch_logs(args.pi_ssh, since, args.journal_cmd)
    d = parse_logs(logs)

    counts = {k: len(v) for k, v in d.items() if k != "send_kind" and k != "verify_kind"}
    print(f"  logs collectés : {counts}")
    n = min(len(d["serialize"]), len(d["deserialize"]), len(d["migration"]),
            len(d["recv"]), len(d["send"]))
    if n == 0:
        print("\n[FAIL] aucun log [MICROBENCH] apparié. Vérifie : rebuild fait ? "
              "journald lisible (sudo) ? une seule fonction sollicitée ?", file=sys.stderr)
        return 1
    if n != args.count:
        print(f"  [warn] {n} requêtes appariées sur {args.count} (logs manquants ou trafic parasite).")

    # ── écriture CSV : un coût (ns) par requête ──────────────────────────────
    rows = []
    for i in range(n):
        sendfd = d["recv"][i] - d["send"][i]     # même horloge CLOCK_MONOTONIC_RAW
        rows.append({
            "req": i + 1,
            "serialize_ns": d["serialize"][i],
            "deserialize_ns": d["deserialize"][i],
            "sendfd_ns": sendfd,
            "migration_ns": d["migration"][i],
            "provider_kind": d["send_kind"][i] if i < len(d["send_kind"]) else "",
            "provider_sendfd_ts": d["send"][i],
            "watchdog_recvfd_ts": d["recv"][i],
        })
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # ── résumé moy / p50 / p99 (µs) ──────────────────────────────────────────
    print(f"\n  {'coût':<14}{'moy(µs)':>10}{'p50(µs)':>10}{'p99(µs)':>10}")
    for key, label in [("serialize_ns", "serialize"), ("deserialize_ns", "deserialize"),
                       ("sendfd_ns", "sendfd"), ("migration_ns", "migration")]:
        xs = [r[key] for r in rows]
        avg = sum(xs) / len(xs)
        print(f"  {label:<14}{avg/1000:>10.1f}{_pctl(xs,0.5)/1000:>10.1f}{_pctl(xs,0.99)/1000:>10.1f}")
    print(f"\n[ok] {n} lignes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
