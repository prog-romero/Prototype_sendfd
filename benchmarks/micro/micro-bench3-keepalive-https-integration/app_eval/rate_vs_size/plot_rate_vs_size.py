#!/usr/bin/env python3
"""
Plot : RPS max (sans erreur) par taille d'image — proto vs vanilla.

Rendu : un **histogramme groupé** (une barre Vanilla + une barre Prototype par
taille d'image), avec en plus une **courbe** par mode qui relie le sommet des
barres, et la valeur annotée au-dessus de chaque barre.

Lit les CSV RÉSUMÉ produits par run_rate_vs_size.py (colonnes mode, size_kb,
max_rps_no_error). On peut passer autant de fichiers que l'on veut : les lignes
sont regroupées **par mode** (une barre/courbe par mode), et si une taille
apparaît plusieurs fois pour un mode, la dernière valeur lue est gardée.

Usage
-----
  python3 plot_rate_vs_size.py results/vanilla_http.csv results/proto_http.csv \\
      --output plots/max_rps_vs_size_http.png

  # un fichier par taille (glob) — regroupé par mode automatiquement :
  python3 plot_rate_vs_size.py results/vanilla/*.csv results/proto/*.csv \\
      --output plots/max_rps_vs_size_http.png
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

# Couleur, marqueur de courbe, libellé de légende, par mode.
_STYLE = {
    "vanilla": ("#FF9800", "s", "Vanilla  (proxy HTTP complet)"),
    "proto":   ("#2196F3", "o", "Prototype  (sendfd / migration TLS)"),
}
# Ordre d'affichage des barres (gauche -> droite).
_ORDER = ["vanilla", "proto"]


def main() -> None:
    ap = argparse.ArgumentParser(description="Histogramme + courbe : RPS max sans erreur vs taille d'image.")
    ap.add_argument("csvs", nargs="+", help="CSV résumé(s) — regroupés par mode")
    ap.add_argument("--output", default="plots/max_rps_vs_size.png")
    ap.add_argument("--title", default="RPS max (sans erreur) par taille d'image")
    args = ap.parse_args()

    # Regroupe par mode : {mode: {size_kb: {"rps", "cpu_avg", "cpu_max", "net"}}}.
    # cpu_* (échelle 400%) et net (% carte réseau) sont relevés au palier rate_at_max.
    def _g(row, key):
        try:
            return float(row.get(key, "") or 0.0)
        except (ValueError, TypeError):
            return 0.0

    by_mode: dict[str, dict[int, dict]] = {}
    for path in args.csvs:
        with open(path) as f:
            for r in csv.DictReader(f):
                try:
                    mode = r["mode"]
                    size = int(r["size_kb"])
                    rps = float(r["max_rps_no_error"])
                except (KeyError, ValueError):
                    continue
                # net_mbit_s (Mbit/s) -> divisé par 8 pour avoir des MB/s
                net_mb_s = _g(r, "net_mbit_s") / 8.0
                by_mode.setdefault(mode, {})[size] = {
                    "rps": rps,
                    "cpu_avg": _g(r, "cpu_avg_pct"),
                    "cpu_max": _g(r, "cpu_max_pct"),
                    "net_mb_s": net_mb_s,
                }

    if not by_mode:
        print("Aucune donnée valide dans les CSV fournis.", file=sys.stderr)
        sys.exit(1)

    # Axe X = union triée de toutes les tailles rencontrées.
    sizes = sorted({s for d in by_mode.values() for s in d})
    x = np.arange(len(sizes), dtype=float)
    x_labels = [str(s) for s in sizes]

    # Modes à tracer : ceux connus dans l'ordre voulu, puis d'éventuels autres.
    modes = [m for m in _ORDER if m in by_mode] + [m for m in by_mode if m not in _ORDER]
    n = len(modes)
    width = 0.8 / max(n, 1)   # largeur de barre selon le nombre de modes

    # Figure plus haute pour laisser de la place aux annotations multi-lignes.
    fig, ax = plt.subplots(figsize=(15, 8.5))

    for i, mode in enumerate(modes):
        color, marker, label = _STYLE.get(mode, ("#444444", "^", mode))
        recs = [by_mode[mode].get(s) for s in sizes]
        vals = np.array([(r["rps"] if r else 0.0) for r in recs])
        offset = (i - (n - 1) / 2.0) * width
        xb = x + offset

        ax.bar(xb, vals, width=width * 0.95, color=color, alpha=0.65,
               label=label, edgecolor="white", linewidth=0.5, zorder=2)
        ax.plot(xb, vals, color=color, marker=marker, linewidth=2,
                markersize=6, zorder=3)

        # Décalage horizontal de l'annotation pour éviter le chevauchement :
        # vanilla (i=0) → annotation décalée à GAUCHE ; proto (i=1) → à DROITE.
        # Quand il n'y a qu'un seul mode, on centre.
        if n == 1:
            x_pts, ha = 0, "center"
        elif i == 0:
            x_pts, ha = -18, "right"   # vanilla : à gauche de sa barre
        else:
            x_pts, ha = 18, "left"     # proto   : à droite de sa barre

        for xi, v, r in zip(xb, vals, recs):
            if v > 0 and r:
                # 3 lignes :  RPS / CPU avg/max % (400 %) / réseau en MB/s
                txt = (f"{v:.1f} rps\n"
                       f"C {r['cpu_avg']:.0f}/{r['cpu_max']:.0f}%\n"
                       f"N {r['net_mb_s']:.2f} MB/s")
                ax.annotate(
                    txt, (xi, v),
                    textcoords="offset points",
                    xytext=(x_pts, 6),
                    ha=ha, va="bottom",
                    fontsize=7.5, linespacing=1.4,
                    color=color, zorder=5,
                    bbox=dict(boxstyle="round,pad=0.2", fc="white",
                              ec=color, alpha=0.75, linewidth=0.6),
                )

    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, fontsize=11)
    ax.set_xlabel("Taille de l'image (KB)", fontsize=12, labelpad=8)
    ax.set_ylabel("RPS max sans erreur (req/s)", fontsize=12, labelpad=8)
    ax.set_title(args.title, fontsize=14, fontweight="bold", pad=12)
    ax.grid(axis="y", linestyle="--", alpha=0.35, zorder=0)
    ax.legend(frameon=False, fontsize=11, loc="upper right")

    # Marge verticale généreuse pour les boîtes d'annotation (3 lignes ~60pt).
    top = max((r["rps"] for d in by_mode.values() for r in d.values()), default=1.0)
    ax.set_ylim(0, top * 1.45)

    # Note explicative en bas de la figure.
    ax.text(0.005, 0.995,
            "annotations  •  C avg/max % = CPU Pi (sur 400 % = 4 cœurs)"
            "  •  N MB/s = débit réseau réel (upload image + download réponse)",
            transform=ax.transAxes, ha="left", va="top", fontsize=8,
            style="italic", color="#444444")

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] figure → {out}")


if __name__ == "__main__":
    main()
