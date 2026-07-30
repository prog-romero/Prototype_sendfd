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

# Article style (font + figure size).
plt.rcParams['figure.figsize'] = 12, 5
plt.rcParams['font.size'] = 18
plt.rcParams['font.family'] = 'Nimbus Roman'

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
    "gateway":   "Gateway",
    "faasd":     "FaaS Provider",
    "container": "Container",
    "autres":    "Others",
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
    ap.add_argument("--out", default="results/bars.pdf")
    ap.add_argument("--format", choices=["png", "pdf", "both"], default=None,
                    help="format de sortie : png, pdf, ou both (déf: déduit de l'extension de --out)")
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
    fig, ax = plt.subplots()                           # figure size from rcParams (12, 5)
    x = np.arange(len(runtimes))
    n = len(COMPONENTS)
    width = 0.78 / n                                   # width of one bar
    bar_w = width * 0.88                               # small gap between bars (spacer)

    # y-axis is scaled to the LARGEST single component value (not a fixed 0-100),
    # so the per-component differences are easy to read.
    ymax = max(data[rt][comp] for rt in runtimes for comp in COMPONENTS)

    for i, comp in enumerate(COMPONENTS):
        offs = x + (i - (n - 1) / 2.0) * width
        vals = [data[rt][comp] for rt in runtimes]
        bars = ax.bar(offs, vals, bar_w, color=COLORS[comp], label=LABELS[comp],
                      edgecolor="white", linewidth=0.8, zorder=3)
        # Value on top of each bar.
        for b, v in zip(bars, vals):
            if v >= 0.3:
                ax.text(b.get_x() + b.get_width() / 2, v + ymax * 0.01, f"{v:.0f}",
                        ha="center", va="bottom", fontsize=12, color="#0b0b0b")

    # Global CPU (~sar total) per runtime, as a clean aligned row above the bars.
    total_y = ymax * 1.10
    for xi, rt in zip(x, runtimes):
        tot = sum(data[rt].values())
        ax.text(xi, total_y, f"{tot:.0f}%", ha="center", va="bottom",
                fontsize=14, color="#52514e")

    ax.set_xticks(x)
    ax.set_xticklabels([RUNTIME_LABELS.get(rt, rt) for rt in runtimes])
    ax.set_xlabel("Programming Language")
    ax.set_ylabel("CPU (%)")
    ax.set_ylim(0, ymax * 1.22)                        # headroom for the value + total labels
    ax.set_axisbelow(True)
    ax.grid(axis="y", color="#e6e6e2", linewidth=1, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.spines["left"].set_color("#c3c2b7")
    ax.spines["bottom"].set_color("#c3c2b7")
    ax.tick_params(length=0)

    mode_lbl = "Vanilla" if args.mode == "vanilla" else "Prototype (sendfd)"
    suffix = "" if args.min_rate <= 0 else f"  (rate >= {args.min_rate:g})"
    #ax.set_title(f"CPU per component and per runtime - {mode_lbl}{suffix}", pad=34)

    # Legend at the TOP (above the bars), horizontal, one entry per component.
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=len(COMPONENTS),
              frameon=False, fontsize=14, columnspacing=1.2, handletextpad=0.5)
    fig.tight_layout()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Output format: --format wins (png / pdf / both); otherwise it is inferred
    # from the extension of --out (so --out foo.png -> PNG, --out foo.pdf -> PDF).
    if args.format == "both":
        exts = [".png", ".pdf"]
    elif args.format:
        exts = ["." + args.format]
    else:
        exts = [out.suffix or ".pdf"]

    for ext in exts:
        dst = out.with_suffix(ext)
        # Requested savefig call is kept verbatim; fancybox is a legend option
        # (not a savefig one) and raises TypeError on recent matplotlib, so we
        # fall back without it to never fail.
        try:
            plt.savefig(dst, pad_inches=0, bbox_inches='tight', fancybox=True)
        except TypeError:
            plt.savefig(dst, pad_inches=0, bbox_inches='tight')
        print(f"[ok] barres → {dst}")
    plt.close(fig)


if __name__ == "__main__":
    main()
