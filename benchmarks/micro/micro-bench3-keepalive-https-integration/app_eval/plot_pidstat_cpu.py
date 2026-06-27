#!/usr/bin/env python3
"""
plot_pidstat_cpu.py — Visualise le CPU TOTAL (cpu_pct) par composant, vanilla vs
prototype, à partir des CSV pidstat produits par sweep_app_wrk2.py.

Source des données
------------------
  pidstat/<mode>/cpu/<composant>.csv   (colonnes : rate, usr_pct, system_pct, cpu_pct)
  <mode>     = "vanilla" ou "prototype"
  <composant> = gateway, faasd, fwatchdog-<fn>, worker-<fn>
On n'utilise QUE la colonne `cpu_pct` (= CPU total du composant à ce débit).

Deux figures sont générées :

  1) cpu_lines.png — DEUX sous-graphes côte à côte (même ligne, MÊME échelle Y) :
       gauche  = CPU de chaque composant de VANILLA en fonction du débit ;
       droite  = idem pour le PROTOTYPE.
     Même couleur = même composant dans les deux sous-graphes → comparaison
     visuelle directe même si ce ne sont pas les mêmes axes.

  2) cpu_pies.png — DEUX camemberts côte à côte :
       gauche  = part de CPU de chaque composant en VANILLA, où la part d'un
                 composant = la MOYENNE de son cpu_pct sur TOUS les débits ;
       droite  = idem pour le PROTOTYPE.

Usage
-----
  python3 plot_pidstat_cpu.py --pidstat-dir pidstat --out-dir plots/pidstat_cpu
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


def load_mode(cpu_dir: Path) -> dict[str, list[tuple[float, float]]]:
    """Charge {composant: [(rate, cpu_pct), ...] trié par rate} pour un mode."""
    out: dict[str, list[tuple[float, float]]] = {}
    if not cpu_dir.is_dir():
        return out
    for csv_path in sorted(cpu_dir.glob("*.csv")):
        comp = csv_path.stem  # nom du composant = nom du fichier sans .csv
        pts: list[tuple[float, float]] = []
        with csv_path.open() as f:
            for r in csv.DictReader(f):
                try:
                    pts.append((float(r["rate"]), float(r["cpu_pct"])))
                except (KeyError, ValueError):
                    continue
        if pts:
            out[comp] = sorted(pts, key=lambda t: t[0])
    return out


def build_color_map(components: list[str]) -> dict[str, tuple]:
    """Une couleur stable par composant (même couleur en vanilla et en proto)."""
    comps = sorted(components)
    cmap = plt.get_cmap("tab20")
    return {c: cmap(i % 20) for i, c in enumerate(comps)}


# ── Figure 1 : courbes CPU vs débit — UNE IMAGE PAR MODE (plus lisible) ───────

def _plot_lines_one(data, title, ylim, colors, out_path: Path) -> None:
    """Trace les courbes CPU d'UN seul mode, en grand, dans son propre fichier."""
    fig, ax = plt.subplots(figsize=(13, 7.5))
    for comp in sorted(data):
        xs = [r for r, _ in data[comp]]
        ys = [c for _, c in data[comp]]
        ax.plot(xs, ys, marker="o", linewidth=2.2, markersize=6,
                color=colors[comp], label=comp)
    ax.set_title(title, fontsize=16, fontweight="bold", pad=12)
    ax.set_xlabel("débit (req/s)", fontsize=13, labelpad=8)
    ax.set_ylabel("CPU total du composant (%)", fontsize=13, labelpad=8)
    ax.grid(True, linestyle="--", alpha=0.35)
    if ylim and ylim > 0:
        ax.set_ylim(0, ylim)   # MÊME échelle Y que l'autre mode → comparable
    ax.legend(loc="center left", bbox_to_anchor=(1.01, 0.5), frameon=False, fontsize=12)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] courbes  → {out_path}")


def plot_lines(vanilla, proto, colors, out_dir: Path) -> None:
    """Deux IMAGES SÉPARÉES (vanilla, prototype) avec la MÊME échelle Y."""
    gmax = 0.0
    for data in (vanilla, proto):
        for comp in data:
            for _, c in data[comp]:
                gmax = max(gmax, c)
    ylim = gmax * 1.10 if gmax > 0 else None
    if vanilla:
        _plot_lines_one(vanilla, "Vanilla — CPU total par composant",
                        ylim, colors, out_dir / "cpu_lines_vanilla.png")
    if proto:
        _plot_lines_one(proto, "Prototype — CPU total par composant",
                        ylim, colors, out_dir / "cpu_lines_prototype.png")


# ── Figure 2 : camemberts de la moyenne du CPU par composant ──────────────────

def plot_pies(vanilla, proto, colors, out_path: Path) -> None:
    # figsize resserré + wspace ~0 pour RAPPROCHER les deux cercles.
    fig, (axv, axp) = plt.subplots(1, 2, figsize=(11, 6.8))

    def draw(ax, data, title):
        comps = sorted(data)
        means = [float(np.mean([c for _, c in data[comp]])) for comp in comps]
        total = sum(means)
        # PAS d'autopct : rien d'écrit DANS le camembert (ça surchargeait).
        wedges, _texts = ax.pie(
            means, colors=[colors[c] for c in comps],
            startangle=90, radius=1.15,
            wedgeprops=dict(edgecolor="white", linewidth=1.2),
        )
        ax.set_title(f"{title}\n(CPU total moyen = {total:.0f} %cpu)",
                     fontsize=13, fontweight="bold", pad=14)
        return wedges, comps

    wedges, comps = draw(axv, vanilla, "Vanilla")
    draw(axp, proto, "Prototype")

    # Rapprocher les deux cercles.
    fig.subplots_adjust(wspace=0.02)

    # GRANDE légende lisible (couleurs + noms bien visibles), sous les camemberts.
    fig.legend(wedges, comps, loc="lower center", ncol=min(len(comps), 4),
               frameon=False, fontsize=13, handlelength=1.6, handleheight=1.4,
               columnspacing=1.6, labelspacing=0.6, bbox_to_anchor=(0.5, -0.08))

    fig.suptitle("Part moyenne de CPU par composant (moyenne sur tous les débits)",
                 fontsize=15, fontweight="bold", y=1.02)
    fig.tight_layout(rect=[0, 0.10, 1, 0.95])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] camemberts → {out_path}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Plots CPU par composant (cpu_pct), vanilla vs proto.")
    ap.add_argument("--pidstat-dir", default="pidstat", help="dossier racine pidstat")
    ap.add_argument("--out-dir", default="plots/pidstat_cpu", help="dossier de sortie des PNG")
    ap.add_argument("--vanilla-name", default="vanilla", help="nom du sous-dossier vanilla")
    ap.add_argument("--proto-name", default="prototype", help="nom du sous-dossier prototype")
    args = ap.parse_args()

    base = Path(args.pidstat_dir)
    vanilla = load_mode(base / args.vanilla_name / "cpu")
    proto = load_mode(base / args.proto_name / "cpu")

    if not vanilla and not proto:
        print(f"ERREUR: aucun CSV trouvé sous {base}/(vanilla|prototype)/cpu/", file=sys.stderr)
        sys.exit(1)

    colors = build_color_map(list(vanilla) + list(proto))
    out_dir = Path(args.out_dir)
    plot_lines(vanilla, proto, colors, out_dir)              # 2 images séparées
    plot_pies(vanilla, proto, colors, out_dir / "cpu_pies.png")


if __name__ == "__main__":
    main()
