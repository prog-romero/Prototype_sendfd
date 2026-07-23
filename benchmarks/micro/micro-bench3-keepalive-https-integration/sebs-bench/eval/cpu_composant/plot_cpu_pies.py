#!/usr/bin/env python3
"""
plot_cpu_pies.py — camembert du CPU par composant, focalisé sur ce qui est
RÉCUPÉRABLE via sendfd : gateway + faasd(provider) + containerd vs le reste.

Idée : dans le chemin VANILLA, la gateway (termine TLS + reverse-proxy de CHAQUE
requête), le provider faasd (résolution + proxy par requête) et containerd (RPC
de résolution) consomment du CPU sur le chemin par-requête. sendfd les
court-circuite après la migration -> ce CPU est rendu au container.

QUATRE parts, agrégées sur tous les débits (moyenne du cpu_pct par composant) :
  - gateway
  - faasd-provider
  - containerd
  - autres           (tout le reste : faasd superviseur, collect, shims, journald,
                      ksoftirqd, fwatchdog, worker, ET le résiduel système)

Échelle : les cpu_pct des CSV sont déjà en % du Pi (pidstat ÷ ncores), donc la
somme des parts = le CPU sar réellement mesuré pendant l'éval.

Source (produite par run_sweep_cpu.py) :
  <pidstat-dir>/<mode>/cpu/<composant>.csv   (colonnes rate,usr_pct,system_pct,cpu_pct)
  <mode> = "vanilla" et/ou "prototype" (les deux -> deux camemberts côte à côte).

Usage :
  python3 plot_cpu_pies.py --pidstat-dir results/pidstat --out plots/cpu_pies.png
  python3 plot_cpu_pies.py --pidstat-dir results/pidstat --out plots/cpu_pies.png --min-rate 512
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

# Parts nommées (chacune sa couleur) ; TOUT le reste -> "autres" (gris neutre),
# y compris le résiduel système. « récupérable via sendfd » = gateway +
# faasd-provider (containerd est montré à part : partiellement récupérable).
NAMED = ["gateway", "faasd-provider", "containerd"]
SLICES = NAMED + ["autres"]
RECLAIMABLE = ["gateway", "faasd-provider"]
COLORS = {
    "gateway": "#1f77b4",
    "faasd-provider": "#ff7f0e",
    "containerd": "#d62728",
    "autres": "#c7c7c7",     # faasd, collect, shims, journald, ksoftirqd, fwatchdog, worker, systeme
}


def _mean_cpu(csv_path: Path, min_rate: float) -> float:
    """Moyenne du cpu_pct d'un composant sur les débits >= min_rate."""
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


def load_mode(cpu_dir: Path, min_rate: float) -> dict[str, float]:
    """Retourne {gateway, faasd-provider, containerd, autres} = CPU moyen (%cpu)."""
    if not cpu_dir.is_dir():
        return {}
    agg = {s: 0.0 for s in SLICES}
    for csv_path in sorted(cpu_dir.glob("*.csv")):
        comp = csv_path.stem     # gateway / faasd-provider / containerd / (tout le reste)
        mean = _mean_cpu(csv_path, min_rate)
        if comp in NAMED:
            agg[comp] += mean
        else:                    # faasd, collect, shims, journald, ksoftirqd, fwatchdog, worker, systeme
            agg["autres"] += mean
    return agg


def draw_pie(ax, agg: dict[str, float], title: str) -> None:
    vals = [agg.get(s, 0.0) for s in SLICES]
    total = sum(vals)
    reclaim = sum(agg.get(s, 0.0) for s in RECLAIMABLE)
    reclaim_pct = (100.0 * reclaim / total) if total > 0 else 0.0

    # Arrondi "plus fort reste" (Hamilton) : les entiers affichés somment
    # EXACTEMENT à 100 % (sinon la somme des arrondis donne parfois 99 ou 101 %).
    raw = [100.0 * v / (total or 1.0) for v in vals]
    ints = [int(x) for x in raw]
    for i in sorted(range(len(raw)), key=lambda k: raw[k] - ints[k], reverse=True)[:100 - sum(ints)]:
        ints[i] += 1
    labels_it = iter(ints)

    # « explode » légèrement les parts récupérables pour les mettre en avant.
    explode = [0.06 if s in RECLAIMABLE else 0.0 for s in SLICES]
    ax.pie(
        vals, colors=[COLORS[s] for s in SLICES],
        explode=explode, startangle=90, radius=1.15,
        autopct=lambda _p: (lambda n: f"{n}%" if n >= 3 else "")(next(labels_it)),
        pctdistance=0.72,
        wedgeprops=dict(edgecolor="white", linewidth=1.2),
        textprops=dict(fontsize=11, fontweight="bold"),
    )
    ax.set_title(f"{title}\nCPU total moyen = {total:.0f} %cpu\n"
                 ,
                 fontsize=12, fontweight="bold", pad=12)


def main() -> None:
    ap = argparse.ArgumentParser(description="Camembert CPU par composant (gateway/faasd-provider/containerd/autres).")
    ap.add_argument("--pidstat-dir", required=True,
                    help="dossier contenant <mode>/cpu/<composant>.csv (vanilla et/ou prototype)")
    ap.add_argument("--out", default="plots/cpu_pies.png", help="image de sortie (.png)")
    ap.add_argument("--min-rate", type=float, default=0.0,
                    help="ne moyenne que les débits >= min-rate (déf 0 = tous ; "
                         "mets ta zone de saturation pour la part 'à charge max')")
    args = ap.parse_args()

    base = Path(args.pidstat_dir)
    modes = [("vanilla", "Vanilla"), ("prototype", "Prototype")]
    available = [(d, title, load_mode(base / d / "cpu", args.min_rate))
                 for d, title in modes]
    available = [(d, title, agg) for d, title, agg in available if agg and sum(agg.values()) > 0]

    if not available:
        print(f"ERREUR: aucun CSV composant trouvé sous {base}/<mode>/cpu/", file=sys.stderr)
        sys.exit(1)

    fig, axes = plt.subplots(1, len(available), figsize=(6.0 * len(available), 6.4))
    if len(available) == 1:
        axes = [axes]
    for ax, (_d, title, agg) in zip(axes, available):
        draw_pie(ax, agg, title)

    # Légende commune sous les camemberts (4 parts).
    handles = [plt.Rectangle((0, 0), 1, 1, color=COLORS[s]) for s in SLICES]
    labels = ["gateway ", "faasd-provider",
              "containerd ", "autres (Container...)"]
    fig.legend(handles, labels, loc="lower center", ncol=4, frameon=False,
               fontsize=11, bbox_to_anchor=(0.5, -0.02))

    suffix = "" if args.min_rate <= 0 else f"  (débits ≥ {args.min_rate:g})"
    fig.suptitle(f"Part de CPU par composant ",
                 fontsize=14, fontweight="bold", y=1.02)
    fig.tight_layout(rect=[0, 0.06, 1, 0.95])

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] camembert → {out}")


if __name__ == "__main__":
    main()
