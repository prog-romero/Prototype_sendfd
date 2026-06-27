#!/usr/bin/env python3
"""
RPS max (sans erreur) par taille d'image — macro-bench BeFaaS IoT.

Idée
----
Pour CHAQUE taille d'image, on balaye une liste de débits croissants (--rates).
Pour chaque débit on lance wrk2 (open-loop, -R<rate>) sur objectrecognition avec
l'image de cette taille, et on regarde s'il y a eu des erreurs.

Le « RPS max sans erreur » d'une taille = le **RPS effectivement atteint le plus
élevé** parmi les paliers de débit qui se sont terminés **sans aucune erreur**
(ni non-2xx, ni socket connect/read/write, ni timeout).

On produit :
  - un CSV DÉTAILLÉ : une ligne par (taille, débit) avec rps atteint + erreurs ;
  - un CSV RÉSUMÉ  : une ligne par taille avec le RPS max sans erreur.
Le plot (plot_rate_vs_size.py) trace le RPS max vs taille, proto vs vanilla.

Exemple
-------
  python3 run_rate_vs_size.py --mode proto --scheme https --host 192.168.2.2 \\
      --sizes 2,4,8,16,32,64,128,256,512,1024 \\
      --rates 2,4,6,8,10,12,16,20,30,40,50 \\
      --concurrency 32 --duration-s 20 \\
      --out results/proto_https.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
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


# ── CPU GLOBAL du Pi (via /proc/stat over ssh) ───────────────────────────────
# Même méthode que app_eval/sweep_app_wrk2.py. Échelle = n_cpus × 100 :
# 4 cœurs totalement occupés = 400 % (et non 0..100 %).

def _start_pi_cpu_sampling(pi_ssh, samples, interval_s=1):
    remote = ("nproc; "
              f"for i in $(seq 1 {samples}); do "
              "awk '/^cpu / {print $2,$3,$4,$5,$6,$7,$8,$9}' /proc/stat; "
              f"sleep {interval_s}; done")
    return subprocess.Popen(["ssh", pi_ssh, remote],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _compute_cpu_busy_stats(text):
    """Renvoie (avg, max, min, n_samples) du CPU busy en % sur l'échelle n_cpus×100."""
    lines = text.strip().splitlines()
    n_cpus = 1
    data = lines
    if lines:
        try:
            c = int(lines[0].strip())
            if c > 0:
                n_cpus, data = c, lines[1:]
        except ValueError:
            pass
    rows = []
    for raw in data:
        parts = raw.strip().split()
        if len(parts) < 8:
            continue
        try:
            rows.append(tuple(int(x) for x in parts[:8]))
        except ValueError:
            continue
    if len(rows) < 2:
        return 0.0, 0.0, 0.0, len(rows)
    busy = []
    for prev, cur in zip(rows, rows[1:]):
        pu, pn, ps, pidle, piow, pirq, psoft, psteal = prev
        cu, cn, cs, cidle, ciow, cirq, csoft, csteal = cur
        prev_busy = pu + pn + ps + pirq + psoft + psteal
        cur_busy = cu + cn + cs + cirq + csoft + csteal
        d_total = (cur_busy + cidle + ciow) - (prev_busy + pidle + piow)
        d_busy = cur_busy - prev_busy
        if d_total <= 0:
            continue
        busy.append((100.0 * n_cpus * d_busy) / d_total)  # échelle 0..n_cpus*100 (400% sur 4 cœurs)
    if not busy:
        return 0.0, 0.0, 0.0, len(rows)
    return round(sum(busy) / len(busy), 2), round(max(busy), 2), round(min(busy), 2), len(rows)


def _bitrate_mbit(rps, image_bytes, transfer_kb_s):
    """Débit réseau réel du Pi (Mbit/s) = upload image (client->Pi) + download réponses (Pi->client).

    - upload   : l'IMAGE est envoyée par le client à chaque requête (rps × taille).
                 wrk2 ne le compte PAS dans Transfer/sec, on le calcule donc ici.
    - download : Transfer/sec de wrk2 = les réponses (petit JSON) reçues par le client.
    - Les appels inter-fonctions restent internes au Pi (bridge) -> hors carte réseau.
    """
    upload = rps * image_bytes * 8.0 / 1e6
    download = transfer_kb_s * 1024.0 * 8.0 / 1e6
    return round(upload, 3), round(download, 3), round(upload + download, 3)


def _run_wrk2(wrk2, lua, url, req_path, image_path, duration_s, timeout_s, threads, conc, rate):
    env = os.environ.copy()
    env["WRK_IMAGE_PATH"] = image_path
    env["WRK_PATH"] = req_path
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


def main() -> None:
    p = argparse.ArgumentParser(description="RPS max sans erreur par taille d'image (BeFaaS IoT).")
    p.add_argument("--mode", choices=["proto", "vanilla"], required=True)
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--port", type=int, default=None, help="override port (def 8443/8080)")
    p.add_argument("--pi-ssh", default="romero@192.168.2.2",
                   help="SSH du Pi pour échantillonner le CPU global (/proc/stat) pendant chaque palier")
    p.add_argument("--nic-max-mbit", type=float, default=940.0,
                   help="débit MAX de la carte réseau du Pi en Mbit/s (mesuré via iperf3, def 940)")
    p.add_argument("--function", default="objectrecognition")
    p.add_argument("--sizes", default="2,4,8,16,32,64,128,256,512,1024",
                   help="tailles d'image KB (img-<KB>kb.jpg)")
    p.add_argument("--rates", default="2,4,6,8,10,12,16,20,30,40,50",
                   help="débits cibles req/s à balayer pour chaque taille")
    p.add_argument("--images-dir", default=None,
                   help="dossier des images (def: ../base_latence/images)")
    p.add_argument("--concurrency", type=int, default=32,
                   help="connexions wrk2 (assez haut pour atteindre le RPS visé)")
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--duration-s", type=int, default=20, help="durée par palier")
    p.add_argument("--timeout-s", type=int, default=30)
    p.add_argument("--pause", type=int, default=3, help="pause entre paliers (s)")
    p.add_argument("--rps-tolerance", type=float, default=0.8,
                   help="AFFICHAGE seulement : marque [SAT] si rps_atteint < tolerance*rate. "
                        "N'influence plus l'arrêt ni le max (on ne s'arrête que sur ERREUR).")
    p.add_argument("--no-stop-on-error", action="store_true",
                   help="ne pas arrêter le balayage d'une taille au 1er palier EN ERREUR "
                        "(balaye alors tous les --rates)")
    p.add_argument("--out", required=True, help="CSV RÉSUMÉ (1 ligne/taille)")
    p.add_argument("--detail-out", default=None,
                   help="CSV DÉTAILLÉ (1 ligne/(taille,débit)). Déf: <out sans .csv>_detail.csv")
    p.add_argument("--append", action="store_true",
                   help="AJOUTER au CSV existant au lieu de l'écraser (workflow taille par taille : "
                        "relancer avec une nouvelle --sizes et le même --out accumule les lignes).")
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    req_path = f"/function/{args.function}"
    url = f"{args.scheme}://{args.host}:{port}{req_path}"

    wrk2 = _find_wrk2()
    script_dir = Path(__file__).resolve().parent
    lua = str(script_dir / "client" / "post_image.lua")
    images_dir = Path(args.images_dir) if args.images_dir else (script_dir.parent / "base_latence" / "images")

    sizes = _parse_int_list(args.sizes)
    rates = sorted(_parse_int_list(args.rates))

    detail_out = args.detail_out or str(Path(args.out).with_suffix("")) + "_detail.csv"

    print(f"=== RPS max sans erreur ({args.mode.upper()} / {args.scheme.upper()}) ===")
    print(f"url        : {url}")
    print(f"images     : {images_dir}")
    print(f"sizes KB   : {sizes}")
    print(f"rates      : {rates}")
    print(f"concurrency: {args.concurrency}   duration: {args.duration_s}s   tol: {args.rps_tolerance}")
    print(f"pi-ssh     : {args.pi_ssh}   NIC max : {args.nic_max_mbit} Mbit/s")
    print(f"résumé     : {args.out}")
    print(f"détail     : {detail_out}\n")

    detail_rows = []
    summary_rows = []

    for size_kb in sizes:
        img = images_dir / f"img-{size_kb}kb.jpg"
        if not img.is_file():
            print(f"[{size_kb:>5} KB] IMAGE INTROUVABLE {img} — passée")
            continue
        image_bytes = img.stat().st_size
        print(f"[{size_kb:>5} KB] ({image_bytes} o)")

        best_rps = 0.0
        best_rate = 0
        best = {}   # métriques CPU/réseau du palier qui donne best_rps
        for rate in rates:
            # CPU GLOBAL du Pi échantillonné pendant TOUTE la fenêtre wrk2 de ce palier.
            cpu_proc = _start_pi_cpu_sampling(args.pi_ssh, max(3, args.duration_s + 2))
            rc, out = _run_wrk2(wrk2, lua, url, req_path, str(img),
                                args.duration_s, args.timeout_s, args.threads,
                                args.concurrency, rate)
            try:
                cpu_out, _ = cpu_proc.communicate(timeout=args.duration_s + args.timeout_s + 30)
            except subprocess.TimeoutExpired:
                cpu_proc.kill()
                cpu_out, _ = cpu_proc.communicate()
            cpu_avg, cpu_max, _cpu_min, _ncpu = _compute_cpu_busy_stats(cpu_out)

            d = _parse(out)
            up_mbit, dn_mbit, nic_mbit = _bitrate_mbit(d["rps"], image_bytes, d["transfer_kb_s"])
            nic_pct = round(100.0 * nic_mbit / args.nic_max_mbit, 2) if args.nic_max_mbit > 0 else 0.0

            server_errors = (d["errors_non2xx"] + d["socket_connect_errors"]
                             + d["socket_read_errors"] + d["socket_write_errors"])
            total_errors = server_errors + d["socket_timeout_errors"]
            has_errors = total_errors > 0
            # On NE décide plus rien sur la tolérance : un palier sans erreur compte
            # pour le max (même s'il sature : le rps plafonne mais reste valide).
            # kept_up n'est plus qu'un indicateur d'AFFICHAGE (SAT = saturé sans erreur).
            kept_up = d["rps"] >= args.rps_tolerance * rate

            flag = "ERR" if has_errors else ("OK " if kept_up else "SAT")
            print(f"   R={rate:<4} -> rps={d['rps']:7.2f}  cpu={cpu_avg:6.0f}/{cpu_max:.0f}%"
                  f"  net={nic_pct:5.1f}%({nic_mbit:.0f}Mb/s)  p99={d['p99_ms']:6.0f}ms"
                  f"  err(non2xx={d['errors_non2xx']},conn={d['socket_connect_errors']},"
                  f"to={d['socket_timeout_errors']})  [{flag}]")

            detail_rows.append({
                "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "mode": args.mode, "scheme": args.scheme, "function": args.function,
                "size_kb": size_kb, "image_bytes": image_bytes,
                "target_rate": rate, "concurrency": args.concurrency,
                "achieved_rps": d["rps"], "transfer_kb_s": d["transfer_kb_s"],
                "cpu_avg_pct": cpu_avg, "cpu_max_pct": cpu_max,
                "upload_mbit_s": up_mbit, "download_mbit_s": dn_mbit,
                "net_mbit_s": nic_mbit, "net_pct": nic_pct,
                "avg_ms": d["avg_ms"], "p99_ms": d["p99_ms"],
                "total_requests": d["total_requests"],
                "errors_non2xx": d["errors_non2xx"],
                "socket_connect_errors": d["socket_connect_errors"],
                "socket_read_errors": d["socket_read_errors"],
                "socket_write_errors": d["socket_write_errors"],
                "socket_timeout_errors": d["socket_timeout_errors"],
                "sustainable": int(not has_errors), "exit_code": rc,
            })

            # Le max retenu = plus haut rps parmi les paliers SANS ERREUR.
            if (not has_errors) and d["rps"] > best_rps:
                best_rps = d["rps"]
                best_rate = rate
                best = {  # on retient le CPU/réseau du palier rate_at_max
                    "cpu_avg_pct": cpu_avg, "cpu_max_pct": cpu_max,
                    "transfer_kb_s": d["transfer_kb_s"],
                    "upload_mbit_s": up_mbit, "download_mbit_s": dn_mbit,
                    "net_mbit_s": nic_mbit, "net_pct": nic_pct,
                }

            # ARRÊT uniquement s'il y a des ERREURS (et plus sur la tolérance/saturation).
            if has_errors and (not args.no_stop_on_error):
                print(f"      (palier en ERREUR -> arrêt du balayage pour {size_kb} KB)")
                break
            if args.pause > 0:
                time.sleep(args.pause)

        print(f"   => RPS max sans erreur ({size_kb} KB) = {best_rps:.2f} (à R={best_rate})"
              f"  | CPU {best.get('cpu_avg_pct', 0):.0f}/{best.get('cpu_max_pct', 0):.0f}%"
              f"  | NET {best.get('net_pct', 0):.1f}%\n")
        summary_rows.append({
            "mode": args.mode, "scheme": args.scheme, "function": args.function,
            "size_kb": size_kb, "image_bytes": image_bytes,
            "max_rps_no_error": round(best_rps, 3), "rate_at_max": best_rate,
            "concurrency": args.concurrency, "duration_s": args.duration_s,
            "nic_max_mbit": args.nic_max_mbit,
            # CPU (échelle 400%) et réseau RELEVÉS AU PALIER rate_at_max :
            "cpu_avg_pct": best.get("cpu_avg_pct", 0.0),
            "cpu_max_pct": best.get("cpu_max_pct", 0.0),
            "transfer_kb_s": best.get("transfer_kb_s", 0.0),
            "upload_mbit_s": best.get("upload_mbit_s", 0.0),
            "download_mbit_s": best.get("download_mbit_s", 0.0),
            "net_mbit_s": best.get("net_mbit_s", 0.0),
            "net_pct": best.get("net_pct", 0.0),
        })

    # écriture des CSV (mode "w" = écrase ; --append = ajoute à la suite)
    def write_rows(path, rows, append):
        if not rows:
            return
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        non_empty = Path(path).exists() and Path(path).stat().st_size > 0
        open_mode = "a" if (append and non_empty) else "w"
        with open(path, open_mode, newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            if open_mode == "w":
                w.writeheader()
            w.writerows(rows)

    write_rows(args.out, summary_rows, args.append)
    write_rows(detail_out, detail_rows, args.append)

    verb = "ajouté à" if args.append else "écrit dans"
    print(f"[ok] résumé {verb} → {args.out}")
    print(f"[ok] détail {verb} → {detail_out}")


if __name__ == "__main__":
    main()
