#!/usr/bin/env python3
"""
plot_latency.py — barres de LATENCE MOYENNE (avant saturation) vanilla vs proto,
un groupe par application.

Pour chaque application tu fournis : un nom, un débit de coupure (le rate à partir
duquel la latence explose, exclu), le CSV vanilla et le CSV proto. Le script :
  - ne garde que les lignes dont rate <= MAXRATE (min-rate <= rate <= MAXRATE),
  - moyenne la colonne lat_avg_ms sur ces lignes,
  - trace un histogramme groupé : abscisse = applications, 2 barres (vanilla, proto),
    ordonnée = latence moyenne (ms).

Format CSV attendu : colonnes `rate` et `lat_avg_ms` (produites par run_*_sweep.py).

Usage :
  python3 plot_latency.py \
    --app dynamic-html 64 \
        ../../../CSV-bench/dynamic-html/dynamic-keepalive/vanilla_https_fin.csv \
        ../../../CSV-bench/dynamic-html/dynamic-keepalive/proto_https_fin.csv \
    --app graph-pagerank 128 <vanilla.csv> <proto.csv> \
    --out latency.png
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERREUR: matplotlib + numpy requis (pip3 install matplotlib numpy)", file=sys.stderr)
    sys.exit(1)

# Couleurs validées (skill dataviz, validate_palette.js — pair orange/bleu : PASS).
COLORS = {"vanilla": "#eb6834", "proto": "#2a78d6"}


def mean_latency(csv_path: str, max_rate: float, min_rate: float, col: str):
    """Moyenne de `col` sur les lignes min_rate <= rate <= max_rate. None si vide."""
    vals = []
    try:
        with open(csv_path) as f:
            for row in csv.DictReader(f):
                try:
                    r = float(row["rate"])
                    if r < min_rate or r > max_rate:
                        continue
                    lat = float(row[col])
                    if lat <= 0:          # 0.0 = mesure manquante (ex: rate=1) -> ignoré
                        continue
                    vals.append(lat)
                except (KeyError, ValueError):
                    pass
    except FileNotFoundError:
        print(f"  [warn] introuvable: {csv_path}", file=sys.stderr)
        return None
    return sum(vals) / len(vals) if vals else None


def auto_cutoff(csv_path: str, col: str, rps_factor: float = 0.95, explode_factor: float = 3.0):
    """Détecte le débit de saturation sur le CSV (vanilla). Cutoff = dernier rate
    où le serveur SUIT le débit (rps >= 0.95*rate) ET la latence n'a pas explosé
    (lat <= 3x la plus petite latence vue). On s'arrête à la 1re saturation.
    Retourne le rate, ou le max si jamais saturé, ou None si illisible."""
    rows = []
    try:
        with open(csv_path) as f:
            for row in csv.DictReader(f):
                try:
                    r = float(row["rate"])
                    rps = float(row.get("rps", "0") or 0)
                    lat = float(row[col])
                    rows.append((r, rps, lat))
                except (KeyError, ValueError):
                    pass
    except FileNotFoundError:
        return None
    rows.sort(key=lambda t: t[0])
    cutoff, seen = None, []
    for r, rps, lat in rows:
        if r <= 0 or lat <= 0:
            continue
        base = min(seen) if seen else lat
        if rps >= rps_factor * r and lat <= explode_factor * base:
            cutoff = r
            seen.append(lat)
        else:
            break                      # première saturation -> on coupe ici
    if cutoff is None and rows:
        cutoff = rows[-1][0]
    return cutoff


def main() -> None:
    ap = argparse.ArgumentParser(description="Barres latence moyenne (avant saturation) vanilla vs proto.")
    ap.add_argument("--app", nargs=4, action="append", required=True,
                    metavar=("NAME", "MAXRATE", "VANILLA_CSV", "PROTO_CSV"),
                    help="une application : nom, débit de coupure (un nombre OU 'auto'), "
                         "CSV vanilla, CSV proto  (répéter --app par application). "
                         "ORDRE des CSV : VANILLA d'abord, PROTO ensuite.")
    ap.add_argument("--sort", action="store_true",
                    help="ordonne les applications par latence vanilla CROISSANTE (plus petite d'abord)")
    ap.add_argument("--min-rate", type=float, default=0.0,
                    help="ignore les rate < min-rate (déf 0 ; utile pour sauter le warm-up rate=1)")
    ap.add_argument("--latency-col", default="lat_avg_ms",
                    help="colonne de latence à moyenner (déf lat_avg_ms)")
    ap.add_argument("--title", default="Latence moyenne avant saturation — vanilla vs prototype")
    ap.add_argument("--out", default="latency.png")
    args = ap.parse_args()

    names, van, pro, gains = [], [], [], []
    for name, maxrate, vcsv, pcsv in args.app:
        # Cutoff : nombre explicite, ou détecté sur le CSV VANILLA si 'auto'.
        if str(maxrate).lower() == "auto":
            mr = auto_cutoff(vcsv, args.latency_col)
            if mr is None:
                print(f"  [skip] {name} : CSV vanilla illisible pour l'auto-cutoff")
                continue
            tag = f"auto={mr:g}"
        else:
            mr = float(maxrate)
            tag = f"{mr:g}"
        v = mean_latency(vcsv, mr, args.min_rate, args.latency_col)
        p = mean_latency(pcsv, mr, args.min_rate, args.latency_col)
        if v is None or p is None:
            print(f"  [skip] {name} : données manquantes (rate {args.min_rate:g}..{mr:g})")
            continue
        names.append(name)
        van.append(v)
        pro.append(p)
        gain = 100.0 * (v - p) / v if v > 0 else 0.0   # % de réduction proto vs vanilla
        gains.append(gain)
        print(f"  {name:22} vanilla={v:8.1f} ms   proto={p:8.1f} ms   "
              f"(-{gain:.0f}%)   rates {args.min_rate:g}..{tag}")

    # Tri optionnel par latence vanilla croissante (plus petite application d'abord).
    if args.sort and names:
        order = sorted(range(len(names)), key=lambda i: van[i])
        names = [names[i] for i in order]
        van = [van[i] for i in order]
        pro = [pro[i] for i in order]
        gains = [gains[i] for i in order]

    if not names:
        print("ERREUR: aucune application exploitable.", file=sys.stderr)
        sys.exit(1)

    # ── figure ────────────────────────────────────────────────────────────────
    x = np.arange(len(names))
    width = 0.38
    fig, ax = plt.subplots(figsize=(2.6 + 1.9 * len(names), 6.0))

    b1 = ax.bar(x - width / 2, van, width * 0.92, color=COLORS["vanilla"],
                label="vanilla", edgecolor="white", linewidth=0.8, zorder=3)
    b2 = ax.bar(x + width / 2, pro, width * 0.92, color=COLORS["proto"],
                label="prototype (sendfd)", edgecolor="white", linewidth=0.8, zorder=3)
    ax.bar_label(b1, fmt="%.0f", padding=3, fontsize=9, color="#0b0b0b")
    ax.bar_label(b2, fmt="%.0f", padding=3, fontsize=9, color="#0b0b0b")

    # Gain (réduction de latence proto vs vanilla) en VERT au-dessus de chaque paire.
    ymax = max(van + pro)
    for xi, v, p, g in zip(x, van, pro, gains):
        sign = "−" if g >= 0 else "+"
        ax.text(xi, max(v, p) + 0.085 * ymax, f"{sign}{abs(g):.0f}%",
                ha="center", va="bottom", fontsize=12, fontweight="bold", color="#188038")

    ax.set_xticks(x)
    ax.set_xticklabels(names, fontsize=11, fontweight="bold")
    ax.set_ylabel("latence moyenne (ms)", fontsize=11)
    ax.set_ylim(0, ymax * 1.30)
    ax.set_axisbelow(True)
    ax.grid(axis="y", color="#e6e6e2", linewidth=1, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.spines["left"].set_color("#c3c2b7")
    ax.spines["bottom"].set_color("#c3c2b7")
    ax.tick_params(length=0)

    ax.set_title(args.title, fontsize=14, fontweight="bold", pad=14)
    ax.legend(loc="upper right", frameon=False, fontsize=11)
    fig.tight_layout()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] barres latence → {out}")


if __name__ == "__main__":
    main()
