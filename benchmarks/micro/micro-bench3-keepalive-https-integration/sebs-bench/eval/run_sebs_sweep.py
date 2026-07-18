#!/usr/bin/env python3
"""
run_sebs_sweep.py — rate-sweep wrk2 sur UNE fonction SeBS (vanilla vs proto).

Pour chaque débit cible (--rates), lance wrk2 en open-loop (POST de l'event JSON)
et relève, PAR PALIER de RPS :
  - RPS, latences, erreurs (parsés de la sortie wrk2) ;
  - CPU + RÉSEAU du Pi via UN SEUL `sar -u -n DEV` (échantillonné en parallèle) :
      * pi_cpu_busy_{avg,med,q3,max}_pct : 100 - %idle, échelle 0-100 (agrégé tous cœurs) ;
      * net_kb_s_{avg,max}               : somme rxkB/s + txkB/s sur eth0 ;
  - PERF-COST sur toutes les requêtes du palier (plus besoin de run_perfcost.py) :
      * client_ms_{avg,p50,p99}  : latence bout-en-bout (histogramme wrk2) ;
      * server_ms_{avg,p50,p99}  : results_time du wrapper SeBS, lu dans le corps
                                   de CHAQUE réponse via le hook Lua (post_json.lua) ;
      * overhead_ms_{avg,p50,p99}: client - server (moyenne exacte ; p50/p99 =
                                   différence d'agrégats -> run_perfcost.py pour l'exact).

Colonnes conservées pour compare_two_csv_plots.py : rate, rps, transfer_kb_s,
pi_cpu_busy_avg_pct, pi_cpu_busy_max_pct, lat_avg_ms, total_requests,
socket_timeout_errors.

Pré-requis : `sar` (paquet sysstat) installé sur le Pi.

Exemple :
  python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 \\
     --function dynamic-html --input ../inputs/dynamic-html.json \\
     --rates 5,10,20,40,60,80 --concurrency 16 --duration-s 20 \\
     --pi-ssh romero@192.168.2.2 --out results/proto_https_dynamic-html.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ── parsers wrk2 (identiques aux autres scripts du bench) ─────────────────────
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
_REQUESTS_IN_RE = re.compile(
    r"(?P<reqs>[0-9,]+)\s+requests in\s+[0-9.]+s,\s+(?P<read>[0-9.]+)(?P<read_u>KB|MB|GB)\s+read"
)
_RPS_RE = re.compile(r"Requests/sec:\s+(?P<rps>[0-9.]+)")
_TRANSFER_RE = re.compile(r"Transfer/sec:\s+(?P<val>[0-9.]+)(?P<unit>KB|MB|GB)")
_ERRORS_RE = re.compile(r"Non-2xx or 3xx responses:\s+(?P<n>[0-9]+)")
_SOCKET_ERRORS_RE = re.compile(
    r"Socket errors:\s*connect\s+(?P<connect>[0-9]+),\s*read\s+(?P<read>[0-9]+),"
    r"\s*write\s+(?P<write>[0-9]+),\s*timeout\s+(?P<timeout>[0-9]+)"
)


def _to_ms(v, u):
    return v / 1000.0 if u == "us" else (v * 1000.0 if u == "s" else v)


def _to_kb(v, u):
    return v if u == "KB" else (v * 1024.0 if u == "MB" else (v * 1024.0 * 1024.0 if u == "GB" else v))


def _f0(raw):
    return 0.0 if raw.strip().lower() in {"nan", "-nan", "+nan"} else float(raw)


def _parse(text: str) -> dict:
    m = _LATENCY_STATS_RE.search(text)
    avg_ms = _to_ms(_f0(m.group("avg")), m.group("avg_u")) if m else 0.0
    pcts = {}
    for pm in _PERCENTILE_RE.finditer(text):
        pcts[f"p{float(pm.group('pct')):g}_ms"] = _to_ms(float(pm.group("val")), pm.group("unit"))
    req_m = _REQUESTS_IN_RE.search(text)
    rps_m = _RPS_RE.search(text)
    tr_m = _TRANSFER_RE.search(text)
    err_m = _ERRORS_RE.search(text)
    sock_m = _SOCKET_ERRORS_RE.search(text)
    return {
        "rps": round(float(rps_m.group("rps")), 3) if rps_m else 0.0,
        "transfer_kb_s": round(_to_kb(float(tr_m.group("val")), tr_m.group("unit")), 3) if tr_m else 0.0,
        "avg_ms": round(avg_ms, 3),
        "p99_ms": round(pcts.get("p99_ms", 0.0), 3),
        "p50_ms": round(pcts.get("p50_ms", 0.0), 3),
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
    for c in (os.path.expanduser("~/wrk2/wrk"), os.path.expanduser("~/wrk2/wrk2")):
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return "wrk2"


# ── CPU + RÉSEAU du Pi via UN SEUL sar (échelle CPU 0-100 % agrégée) ──────────
def _start_pi_sar(pi_ssh, samples, interval_s=1):
    """Lance `sar -u -n DEV` sur le Pi (CPU + réseau en une commande), en
    parallèle de la charge du palier. LC_ALL=C -> format stable à parser."""
    remote = f"LC_ALL=C sar -u -n DEV {interval_s} {samples}"
    return subprocess.Popen(["ssh", pi_ssh, remote],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _compute_sar_stats(text, iface="eth0", drop_first=True):
    """Parse la sortie `sar -u -n DEV`.
    CPU : ligne dont le 2e champ == 'all'  -> busy = 100 - %idle (échelle 0-100).
    NET : ligne dont le 2e champ == iface  -> rxkB/s + txkB/s (somme in+out).
    Retourne (cpu_avg, cpu_med, cpu_q3, cpu_max, net_avg_kb_s, net_max_kb_s).
    Ignore les en-têtes et la ligne finale 'Average:'."""
    cpu_busy, net_sum = [], []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[0].startswith("Average"):
            continue
        tag = parts[1]
        if tag == "all":                      # ligne CPU agrégé -> %idle = dernier champ
            try:
                cpu_busy.append(100.0 - float(parts[-1]))
            except ValueError:
                pass
        elif tag == iface:                    # <time> IFACE rxpck txpck rxkB txkB ...
            try:
                net_sum.append(float(parts[4]) + float(parts[5]))
            except (ValueError, IndexError):
                pass
    if drop_first:                            # jette la 1re seconde (partielle)
        if len(cpu_busy) > 1:
            cpu_busy = cpu_busy[1:]
        if len(net_sum) > 1:
            net_sum = net_sum[1:]

    def _median(xs):
        if not xs:
            return 0.0
        s = sorted(xs)
        n = len(s)
        mid = n // 2
        return round(s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0, 2)

    def _quantile(xs, q):
        if not xs:
            return 0.0
        s = sorted(xs)
        idx = min(len(s) - 1, int(round(q * (len(s) - 1))))
        return round(s[idx], 2)

    def _stats(xs):
        return (round(sum(xs) / len(xs), 2), round(max(xs), 2)) if xs else (0.0, 0.0)

    cpu_avg, cpu_max = _stats(cpu_busy)
    cpu_med = _median(cpu_busy)
    cpu_q3 = _quantile(cpu_busy, 0.75)
    net_avg, net_max = _stats(net_sum)
    return cpu_avg, cpu_med, cpu_q3, cpu_max, net_avg, net_max


# ── perf-cost : agrégation du server_ms (results_time) parsé par le hook Lua ──
def _pctl(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[idx]


def _read_server_ms(perf_file):
    """Lit le fichier rempli par le hook Lua (un results_time µs par ligne, pour
    chaque réponse 200 du palier). Retourne la liste des server_ms en ms."""
    vals = []
    try:
        with open(perf_file) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    vals.append(float(line) / 1000.0)   # µs -> ms
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    return vals


def _run_wrk2(wrk2, lua, url, req_path, body_file, duration_s, timeout_s, threads, conc, rate,
              perf_file=None, conn_close=False):
    env = os.environ.copy()
    env["WRK_BODY_FILE"] = body_file
    env["WRK_PATH"] = req_path
    if perf_file:
        env["WRK_PERF_FILE"] = perf_file      # le hook Lua y logge chaque server_ms
    if conn_close:
        env["WRK_CONN_CLOSE"] = "1"           # mode "initial" : 1 connexion / requête
    actual_threads = min(threads, conc)
    cmd = [wrk2, f"-t{actual_threads}", f"-c{conc}", f"-d{duration_s}s", f"-R{rate}",
           "--timeout", f"{timeout_s}s", "--latency", "-s", lua, url]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=duration_s + timeout_s + 40)
    except subprocess.TimeoutExpired as exc:
        return 124, f"wrk2 process timeout: {exc}"
    except FileNotFoundError:
        return 127, "wrk2 introuvable"
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def _parse_int_list(raw):
    return [int(x.strip()) for x in raw.split(",") if x.strip()]


def main():
    p = argparse.ArgumentParser(description="Rate-sweep wrk2 d'une fonction SeBS (vanilla vs proto).")
    p.add_argument("--mode", choices=["proto", "vanilla"], required=True)
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--port", type=int, default=None, help="override port (def 8443/8080)")
    p.add_argument("--function", required=True, help="nom de la fonction (route)")
    p.add_argument("--input", required=True, help="fichier JSON de l'event à POSTer")
    p.add_argument("--rates", default="5,10,20,40,60,80", help="débits cibles req/s")
    p.add_argument("--concurrency", type=int, default=32)
    p.add_argument("--threads", type=int, default=8)
    p.add_argument("--duration-s", type=int, default=20)
    p.add_argument("--timeout-s", type=int, default=30)
    p.add_argument("--pause", type=int, default=5)
    p.add_argument("--conn-mode", choices=["keepalive", "initial"], default="keepalive",
                   help="keepalive (défaut) = connexions réutilisées ; "
                        "initial = une NOUVELLE connexion par requête (Connection: close)")
    p.add_argument("--pi-ssh", default="romero@192.168.2.2")
    p.add_argument("--out", required=True)
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    req_path = f"/function/{args.function}"
    url = f"{args.scheme}://{args.host}:{port}{req_path}"
    body_file = str(Path(args.input).resolve())
    if not Path(body_file).is_file():
        print(f"ERREUR: event introuvable: {body_file}", file=sys.stderr)
        sys.exit(1)

    wrk2 = _find_wrk2()
    lua = str(Path(__file__).resolve().parent / "client" / "post_json.lua")
    rates = _parse_int_list(args.rates)

    print(f"=== SeBS sweep [{args.mode}/{args.scheme}] {args.function} ===")
    print(f"url   : {url}")
    print(f"event : {body_file}")
    print(f"rates : {rates}   concurrency: {args.concurrency}   duration: {args.duration_s}s")
    print(f"conn  : {args.conn_mode}   (initial = 1 connexion/requête ; keepalive = réutilisées)")
    print(f"out   : {args.out}\n")

    rows = []
    for rate in rates:
        # fichier de collecte du server_ms pour CE palier (vidé à chaque rate)
        perf_file = str(Path(args.out).resolve().parent / f".perf_{args.function}_{rate}.tmp")
        try:
            os.remove(perf_file)
        except FileNotFoundError:
            pass

        # Une seule passe : wrk2 (avec hook server_ms) + sar (CPU/réseau) en même
        # temps. wrk2 renvoie parfois une latence "-nan" pour CERTAINES valeurs de
        # durée (bug de calibration coordinated-omission ; observé pile à -d10s,
        # ni 8/12/15/20). Rien à voir avec le hook ni sar. On DÉCALE alors la durée
        # de +1s à chaque re-run pour éviter la valeur pathologique.
        max_tries = 6
        for attempt in range(max_tries):
            dur = args.duration_s + attempt
            try:
                os.remove(perf_file)
            except FileNotFoundError:
                pass
            sar_proc = _start_pi_sar(args.pi_ssh, max(3, dur))
            rc, out = _run_wrk2(wrk2, lua, url, req_path, body_file,
                                dur, args.timeout_s, args.threads,
                                args.concurrency, rate, perf_file=perf_file,
                                conn_close=(args.conn_mode == "initial"))
            try:
                sar_out, _ = sar_proc.communicate(timeout=dur + args.timeout_s + 30)
            except subprocess.TimeoutExpired:
                sar_proc.kill()
                sar_out, _ = sar_proc.communicate()
            cpu_avg, cpu_med, cpu_q3, cpu_max, net_avg, net_max = _compute_sar_stats(sar_out)
            d = _parse(out)
            if not (d["avg_ms"] == 0.0 and d["total_requests"] > 0):
                break
            if attempt < max_tries - 1:
                print(f"  R={rate:<4} -> latence wrk2 = -nan à {dur}s (bug durée), re-run à {dur + 1}s…")
        server_vals = sorted(_read_server_ms(perf_file))

        # ── perf-cost sur TOUTES les requêtes du palier ──────────────────────
        #  client_ms : latence bout-en-bout côté client   -> histogramme wrk2
        #  server_ms : results_time du wrapper SeBS         -> hook Lua (toutes les 200)
        #  overhead  : client - server. moyenne EXACTE ; p50/p99 = différence
        #              d'agrégats (approx : wrk2 ne permet pas d'apparier
        #              client & server par requête -> run_perfcost.py pour l'exact).
        cli_avg, cli_p50, cli_p99 = d["avg_ms"], d["p50_ms"], d["p99_ms"]
        if server_vals:
            srv_avg = round(sum(server_vals) / len(server_vals), 3)
            srv_p50 = round(_pctl(server_vals, 0.50), 3)
            srv_p99 = round(_pctl(server_vals, 0.99), 3)
        else:
            srv_avg = srv_p50 = srv_p99 = 0.0
        ovh_avg = round(cli_avg - srv_avg, 3)
        ovh_p50 = round(cli_p50 - srv_p50, 3)
        ovh_p99 = round(cli_p99 - srv_p99, 3)
        try:
            os.remove(perf_file)
        except FileNotFoundError:
            pass

        total_errors = (d["errors_non2xx"] + d["socket_connect_errors"]
                        + d["socket_read_errors"] + d["socket_write_errors"]
                        + d["socket_timeout_errors"])
        print(f"  R={rate:<4} -> rps={d['rps']:8.2f}  cpu(avg/med/q3/max)={cpu_avg:5.1f}/{cpu_med:5.1f}/{cpu_q3:5.1f}/{cpu_max:.1f}%"
              f"  net={net_avg:8.1f}kB/s"
              f"  cli/srv/ovh={cli_avg:6.1f}/{srv_avg:6.1f}/{ovh_avg:6.1f}ms"
              f"  err={total_errors}  (n_srv={len(server_vals)})")

        rows.append({
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "mode": args.mode, "scheme": args.scheme, "function": args.function,
            "conn_mode": args.conn_mode,
            "rate": rate, "concurrency": args.concurrency,
            "rps": d["rps"],
            "transfer_kb_s": d["transfer_kb_s"],          # wrk2 (compat plots)
            "net_kb_s_avg": net_avg, "net_kb_s_max": net_max,   # sar eth0 (rx+tx)
            "pi_cpu_busy_avg_pct": cpu_avg, "pi_cpu_busy_med_pct": cpu_med,
            "pi_cpu_busy_q3_pct": cpu_q3, "pi_cpu_busy_max_pct": cpu_max,  # sar 0-100 (avg/méd/Q3/max)
            "lat_avg_ms": d["avg_ms"], "lat_p50_ms": d["p50_ms"], "lat_p99_ms": d["p99_ms"],
            "client_ms_avg": cli_avg, "client_ms_p50": cli_p50, "client_ms_p99": cli_p99,
            "server_ms_avg": srv_avg, "server_ms_p50": srv_p50, "server_ms_p99": srv_p99,
            "overhead_ms_avg": ovh_avg, "overhead_ms_p50": ovh_p50, "overhead_ms_p99": ovh_p99,
            "server_samples": len(server_vals),
            "total_requests": d["total_requests"],
            "errors_non2xx": d["errors_non2xx"],
            "socket_connect_errors": d["socket_connect_errors"],
            "socket_read_errors": d["socket_read_errors"],
            "socket_write_errors": d["socket_write_errors"],
            "socket_timeout_errors": d["socket_timeout_errors"],
            "exit_code": rc,
        })
        if args.pause > 0:
            time.sleep(args.pause)

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\n[ok] écrit → {args.out}")


if __name__ == "__main__":
    main()
