#!/usr/bin/env python3
"""
Plot : RPS max (sans erreur) en fonction de la taille d'image — proto vs vanilla.

Lit les CSV RÉSUMÉ produits par run_rate_vs_size.py (colonnes mode, size_kb,
max_rps_no_error) et trace une courbe par mode.

On peut passer AUTANT de fichiers que l'on veut (workflow taille par taille :
un CSV par taille). Les lignes sont regroupées **par mode** (proto / vanilla) :
une seule courbe par mode, peu importe le nombre de fichiers. Si une taille
apparaît plusieurs fois pour un mode, la dernière valeur lue est gardée.

Usage
-----
  # un fichier accumulé par mode :
  python3 plot_rate_vs_size.py results/proto_https.csv results/vanilla_https.csv \\
      --output plots/max_rps_vs_size_https.png

  # un fichier par taille (glob) :
  python3 plot_rate_vs_size.py results/proto/*.csv results/vanilla/*.csv \\
      --output plots/max_rps_vs_size_https.png
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("ERREUR: matplotlib requis (pip3 install matplotlib)", file=sys.stderr)
    sys.exit(1)

_STYLE = {
    "proto":   ("#2196F3", "o", "Prototype  (sendfd / migration TLS)"),
    "vanilla": ("#FF9800", "s", "Vanilla  (proxy HTTP complet)"),
}


def main() -> None:
    ap = argparse.ArgumentParser(description="Plot RPS max sans erreur vs taille d'image.")
    ap.add_argument("csvs", nargs="+", help="CSV résumé(s) — autant qu'on veut, regroupés par mode")
    ap.add_argument("--output", default="plots/max_rps_vs_size.png")
    ap.add_argument("--title", default="RPS max (sans erreur) vs taille d'image")
    ap.add_argument("--linear-x", action="store_true", help="axe X linéaire (def: log2)")
    args = ap.parse_args()

    # Regroupe TOUTES les lignes de TOUS les fichiers par mode -> {mode: {size_kb: max_rps}}
    # (dernière valeur lue gardée si une taille apparaît plusieurs fois pour un mode).
    by_mode: dict[str, dict[int, float]] = {}
    for path in args.csvs:
        with open(path) as f:
            for r in csv.DictReader(f):
                try:
                    mode = r["mode"]
                    size = int(r["size_kb"])
                    rps = float(r["max_rps_no_error"])
                except (KeyError, ValueError):
                    continue
                by_mode.setdefault(mode, {})[size] = rps

    if not by_mode:
        print("Aucune donnée valide dans les CSV fournis.", file=sys.stderr)
        sys.exit(1)

    fig, ax = plt.subplots(figsize=(10, 6))
    all_sizes = set()

    for mode in sorted(by_mode):
        pts = sorted(by_mode[mode].items())          # [(size, rps), ...] trié par taille
        xs = [s for s, _ in pts]
        ys = [v for _, v in pts]
        all_sizes.update(xs)
        color, marker, label = _STYLE.get(mode, ("#444444", "^", mode))
        ax.plot(xs, ys, color=color, marker=marker, linewidth=2, markersize=7, label=label)
        for x, y in zip(xs, ys):
            ax.annotate(f"{y:.0f}", (x, y), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=8)

    if not args.linear_x:
        ax.set_xscale("log", base=2)
        sizes_sorted = sorted(all_sizes)
        ax.set_xticks(sizes_sorted)
        ax.set_xticklabels([str(s) for s in sizes_sorted])

    ax.set_xlabel("Taille de l'image (KB)", fontsize=12, labelpad=8)
    ax.set_ylabel("RPS max sans erreur (req/s)", fontsize=12, labelpad=8)
    ax.set_title(args.title, fontsize=14, fontweight="bold", pad=12)
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.legend(frameon=False, fontsize=10)

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] figure → {out}")


if __name__ == "__main__":
    main()
