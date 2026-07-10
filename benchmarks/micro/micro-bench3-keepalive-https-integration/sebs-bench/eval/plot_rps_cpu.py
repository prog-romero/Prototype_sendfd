#!/usr/bin/env python3
"""
plot_rps_cpu.py — RPS (axe gauche) + CPU moyen (axe droit) vs rate, par application.

Pour chaque application : UNE figure à DEUX axes Y.
  - Axe GAUCHE  : RPS atteint.
  - Axe DROIT   : CPU moyen du Pi (%).
  - Axe X       : le rate cible.

À chaque rate, 4 bâtons épais côte à côte :
    [ RPS Vanilla (rouge) | RPS Prototype (vert) ]  ␣petit espace␣  [ CPU Vanilla (orange) | CPU Prototype (bleu) ]

Sortie : results/<app>/rps_cpu_<scheme>/<app>_<scheme>_rps_cpu.png

Usage :
  python3 plot_rps_cpu.py --scheme https                  # toutes les apps
  python3 plot_rps_cpu.py --scheme https --app dynamic-html
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
except ImportError:
    print("ERREUR: matplotlib, numpy, pandas requis. pip3 install matplotlib numpy pandas",
          file=sys.stderr)
    sys.exit(1)

APPS = ["dynamic-html", "graph-pagerank", "thumbnailer", "compression"]

# Couleurs demandées
C_RPS_VAN = "red"        # RPS  vanilla
C_RPS_PRO = "green"      # RPS  prototype
C_CPU_VAN = "orange"     # CPU  vanilla
C_CPU_PRO = "royalblue"  # CPU  prototype

W = 0.10                 # épaisseur des bâtons (épaisseur d'origine 0.20 ÷ 2)


def find_csv(app_dir: Path, mode: str, scheme: str) -> Path | None:
    cands = sorted(app_dir.glob(f"{mode}_{scheme}*.csv"), reverse=True)
    for c in cands:
        try:
            cols = pd.read_csv(c, nrows=0).columns
        except Exception:
            continue
        if "rps" in cols and "pi_cpu_busy_avg_pct" in cols:
            return c
    return cands[0] if cands else None


def prepare(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    return df.sort_values("rate").drop_duplicates(subset=["rate"], keep="last")


def plot_app(app_dir: Path, app: str, scheme: str,
             van: Path | None = None, pro: Path | None = None) -> None:
    # fichiers explicites (--vanilla/--proto) sinon auto-découverte
    van = van or find_csv(app_dir, "vanilla", scheme)
    pro = pro or find_csv(app_dir, "proto", scheme)
    if not van or not pro:
        print(f"  [skip] {app}: paire {scheme} introuvable (vanilla={bool(van)}, proto={bool(pro)})")
        return

    # outer join : on garde TOUS les rates présents dans l'un OU l'autre fichier
    # (aucun rate n'est sauté ; une valeur manquante d'un mode = barre absente).
    m = pd.merge(prepare(van), prepare(pro), on="rate", suffixes=("_v", "_p"), how="outer").sort_values("rate")
    if m.empty:
        print(f"  [skip] {app}: aucun rate")
        return

    rates = m["rate"].astype(int).astype(str).to_list()
    x = np.arange(len(rates), dtype=float)

    fig, ax = plt.subplots(figsize=(max(9, 1.2 * len(rates)), 5.6))
    ax2 = ax.twinx()   # second axe Y (droite), partage le même axe X

    # RPS (axe gauche) : courbes pleines · CPU (axe droit) : courbes tiretées
    l1, = ax.plot(x, m["rps_v"], marker="o", linewidth=2, color=C_RPS_VAN, label="RPS Vanilla")
    l2, = ax.plot(x, m["rps_p"], marker="o", linewidth=2, color=C_RPS_PRO, label="RPS Prototype")
    l3, = ax2.plot(x, m["pi_cpu_busy_avg_pct_v"], marker="s", linestyle="--", linewidth=2, color=C_CPU_VAN, label="CPU Vanilla")
    l4, = ax2.plot(x, m["pi_cpu_busy_avg_pct_p"], marker="s", linestyle="--", linewidth=2, color=C_CPU_PRO, label="CPU Prototype")

    ax.set_xlabel("rate (Target Requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel("RPS atteint", fontsize=11, labelpad=8)
    ax2.set_ylabel("CPU moyen (%)", fontsize=11, labelpad=8)
    ax.set_xticks(x)
    ax.set_xticklabels(rates)
    ax.set_ylim(bottom=0)
    ax2.set_ylim(bottom=0)
    ax.grid(axis="y", alpha=0.2, linestyle="--")

    # légende commune (les 2 axes réunis)
    handles = [l1, l2, l3, l4]
    ax.legend(handles, [h.get_label() for h in handles],
              frameon=False, fontsize=9, ncol=2, loc="upper left")
    ax.set_title(f"{app} — RPS & CPU moyen : Vanilla vs Prototype", fontsize=13, fontweight="bold", pad=12)

    fig.tight_layout()
    out_dir = app_dir / f"rps_cpu_{scheme}"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{app}_{scheme}_rps_cpu.png"
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"  [ok] {app}: {out_path}  (vanilla={van.name}, proto={pro.name})")


def main() -> None:
    p = argparse.ArgumentParser(description="RPS + CPU (double axe) vs rate, Vanilla vs Prototype.")
    p.add_argument("--results-dir", default=str(Path(__file__).resolve().parent / "results"))
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--app", default=None, help="une seule application (def: toutes)")
    p.add_argument("--vanilla", default=None, help="chemin exact du CSV vanilla (override auto-découverte)")
    p.add_argument("--proto", default=None, help="chemin exact du CSV proto (override auto-découverte)")
    args = p.parse_args()

    # Mode fichiers explicites : on trace juste cette paire.
    if args.vanilla and args.proto:
        van, pro = Path(args.vanilla).resolve(), Path(args.proto).resolve()
        app_dir = van.parent
        app = app_dir.name
        print(f"=== RPS+CPU [{args.scheme}] {app} (fichiers explicites) ===")
        plot_app(app_dir, app, args.scheme, van=van, pro=pro)
        return

    root = Path(args.results_dir).expanduser().resolve()
    apps = [args.app] if args.app else APPS
    print(f"=== RPS+CPU [{args.scheme}] depuis {root} ===")
    for app in apps:
        app_dir = root / app
        if not app_dir.is_dir():
            print(f"  [skip] {app}: dossier absent")
            continue
        plot_app(app_dir, app, args.scheme)


if __name__ == "__main__":
    main()
