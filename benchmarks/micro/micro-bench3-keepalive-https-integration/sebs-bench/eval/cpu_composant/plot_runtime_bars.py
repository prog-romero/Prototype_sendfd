#!/usr/bin/env python3
"""
plot_runtime_bars.py — diagramme à barres GROUPÉES du CPU par composant, un
groupe par runtime (C / JS / Python), pour UN mode (vanilla ou prototype).

4 composantes (une couleur chacune), sommées depuis les CSV pidstat par composant
(déjà en % du Pi) :
  - gateway    : gateway.csv
  - faasd      : faasd + faasd-provider + faasd-collect
  - container  : fwatchdog-<fn> + worker-<fn>   (le watchdog + la fonction)
  - autres     : le reste (containerd, containerd-shim, journald, ksoftirqd, systeme)
Le total d'un groupe = le CPU sar du Pi (~96 %).

Source : <results-dir>/<runtime>/pidstat/<mode>/cpu/*.csv
  <runtime> ∈ {sum-c, sum-js, sum-python}, <mode> = vanilla | prototype.

Usage :
  python3 plot_runtime_bars.py --mode vanilla   --out results/bars_vanilla.png
  python3 plot_runtime_bars.py --mode prototype --out results/bars_prototype.png
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

# Composantes (ordre = ordre des barres) + couleurs validées (skill dataviz,
# validate_palette.js) : orange / aqua / bleu / gris. L'ordre a été choisi pour
# que chaque paire adjacente passe les gates CVD ; le gris est le bucket « Other ».
COMPONENTS = ["gateway", "faasd", "container", "autres"]
COLORS = {
    "gateway":   "#eb6834",   # orange
    "faasd":     "#1baf7a",   # aqua
    "container": "#2a78d6",   # bleu
    "autres":    "#9e9e9e",   # gris neutre (le reste du CPU)
}
LABELS = {
    "gateway":   "gateway",
    "faasd":     "faasd",
    "container": "container (fwatchdog + fonction)",
    "autres":    "autres",
}
# Dossier runtime -> libellé affiché.
RUNTIME_LABELS = {"sum-c": "C", "sum-js": "JS", "sum-python": "Python"}


def _mean_cpu(csv_path: Path, min_rate: float) -> float:
    vals = []
    try:
        with open(csv_path) as f:
            for row in csv.DictReader(f):
                try:
                    if float(row["rate"]) < min_rate:
                        continue
                    vals.append(float(row["cpu_pct"]))
                except (KeyError, ValueError):
                    pass
    except FileNotFoundError:
        return 0.0
    return sum(vals) / len(vals) if vals else 0.0


def _bucket(stem: str) -> str:
    if stem == "gateway":
        return "gateway"
    if stem.startswith("faasd"):                       # faasd, faasd-provider, faasd-collect
        return "faasd"
    if stem.startswith("fwatchdog-") or stem.startswith("worker-"):
        return "container"
    return "autres"                                    # containerd, shim, journald, ksoftirqd, systeme


def load_runtime(cpu_dir: Path, min_rate: float):
    """Retourne {composante: %cpu} pour un runtime/mode, ou None si pas de données."""
    if not cpu_dir.is_dir():
        return None
    agg = {c: 0.0 for c in COMPONENTS}
    found = False
    for csv_path in sorted(cpu_dir.glob("*.csv")):
        found = True
        agg[_bucket(csv_path.stem)] += _mean_cpu(csv_path, min_rate)
    return agg if found else None


def main() -> None:
    ap = argparse.ArgumentParser(description="Barres groupées CPU par composant, un groupe par runtime.")
    ap.add_argument("--results-dir", default="results",
                    help="dossier contenant sum-c/ sum-js/ sum-python/ (déf: results)")
    ap.add_argument("--mode", choices=["vanilla", "prototype"], default="vanilla")
    ap.add_argument("--runtimes", default="sum-c,sum-js,sum-python",
                    help="dossiers runtime, séparés par des virgules (ordre d'affichage)")
    ap.add_argument("--min-rate", type=float, default=0.0,
                    help="ne moyenne que les débits >= min-rate (déf 0 = tous)")
    ap.add_argument("--out", default="results/bars.png")
    args = ap.parse_args()

    base = Path(args.results_dir)
    runtimes, data = [], {}
    for rt in [r.strip() for r in args.runtimes.split(",") if r.strip()]:
        agg = load_runtime(base / rt / "pidstat" / args.mode / "cpu", args.min_rate)
        if agg is not None and sum(agg.values()) > 0:
            runtimes.append(rt)
            data[rt] = agg
        else:
            print(f"  [skip] {rt} : pas de données {args.mode}")

    if not runtimes:
        print(f"ERREUR: aucune donnée sous {base}/<runtime>/pidstat/{args.mode}/cpu/", file=sys.stderr)
        sys.exit(1)

    # ── figure ────────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(2.6 + 2.3 * len(runtimes), 6.2))
    x = np.arange(len(runtimes))
    n = len(COMPONENTS)
    width = 0.78 / n                                   # largeur d'une barre
    bar_w = width * 0.88                               # léger écart entre barres (spacer)

    for i, comp in enumerate(COMPONENTS):
        offs = x + (i - (n - 1) / 2.0) * width
        vals = [data[rt][comp] for rt in runtimes]
        bars = ax.bar(offs, vals, bar_w, color=COLORS[comp], label=LABELS[comp],
                      edgecolor="white", linewidth=0.8, zorder=3)
        # Label de valeur au-dessus de chaque barre (encodage secondaire + lisibilité).
        for b, v in zip(bars, vals):
            if v >= 0.3:
                ax.text(b.get_x() + b.get_width() / 2, v + 0.8, f"{v:.0f}",
                        ha="center", va="bottom", fontsize=9, color="#0b0b0b")

    # Total (~sar) au-dessus de chaque groupe.
    for xi, rt in zip(x, runtimes):
        tot = sum(data[rt].values())
        ax.text(xi, max(sum(data[r].values()) for r in runtimes) + 6, f"total {tot:.0f} %",
                ha="center", va="bottom", fontsize=10, fontweight="bold", color="#52514e")

    ax.set_xticks(x)
    ax.set_xticklabels([RUNTIME_LABELS.get(rt, rt) for rt in runtimes], fontsize=12, fontweight="bold")
    ax.set_ylabel("CPU %", fontsize=11)
    ax.set_ylim(0, 100)
    ax.set_axisbelow(True)
    ax.grid(axis="y", color="#e6e6e2", linewidth=1, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.spines["left"].set_color("#c3c2b7")
    ax.spines["bottom"].set_color("#c3c2b7")
    ax.tick_params(length=0)

    mode_lbl = "Vanilla" if args.mode == "vanilla" else "Prototype (sendfd)"
    suffix = "" if args.min_rate <= 0 else f"  (débits ≥ {args.min_rate:g})"
    ax.set_title(f"CPU par composant et par runtime — {mode_lbl}{suffix}",
                 fontsize=14, fontweight="bold", pad=14)

    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.09), ncol=2,
              frameon=False, fontsize=10)
    fig.tight_layout(rect=[0, 0.04, 1, 1])

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] barres → {out}")


if __name__ == "__main__":
    main()
