#!/usr/bin/env python3
"""
plot_rps_cpu.py — métrique de charge (axe gauche) + CPU moyen (axe droit) vs rate.

Pour chaque application, DEUX figures sont générées (axe CPU commun, à droite,
gradué de 0 à 100 %) :
  - <app>_<scheme>_rps_cpu.png : RPS atteint          (gauche) + CPU (droite)
  - <app>_<scheme>_lat_cpu.png : latence moyenne (ms) (gauche) + CPU (droite)

Axe gauche : rouge = Vanilla, vert = Prototype.
Axe droit  : CPU du Pi (moyen OU médian, cf. --cpu-stat), orange = Vanilla,
             bleu = Prototype (tireté), gradué 0-100 %.

Sortie : results/<app>/rps_cpu_<scheme>/<app>_<scheme>_{rps,lat}_cpu[_med].png
  (le suffixe _med n'apparaît que pour --cpu-stat med, pour ne pas écraser l'avg)

Usage :
  python3 plot_rps_cpu.py --scheme https                       # CPU moyen (défaut)
  python3 plot_rps_cpu.py --scheme https --cpu-stat med        # CPU médian
  python3 plot_rps_cpu.py --scheme https --cpu-stat q3         # CPU 3e quartile
  python3 plot_rps_cpu.py --scheme https --vanilla V.csv --proto P.csv
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MultipleLocator
    import numpy as np
    import pandas as pd
except ImportError:
    print("ERREUR: matplotlib, numpy, pandas requis. pip3 install matplotlib numpy pandas",
          file=sys.stderr)
    sys.exit(1)

APPS = ["dynamic-html", "graph-pagerank", "thumbnailer", "compression"]

# Couleurs
C_LEFT_VAN = "red"        # métrique gauche (RPS ou latence) — vanilla
C_LEFT_PRO = "green"      # métrique gauche (RPS ou latence) — prototype
C_CPU_VAN = "orange"      # CPU vanilla
C_CPU_PRO = "royalblue"   # CPU prototype

CPU_YMAX = 100            # graduation de l'axe CPU (droite) : toujours jusqu'à 100


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


def _plot_metric(app_dir: Path, app: str, scheme: str, m: pd.DataFrame,
                 rates: list[str], x: np.ndarray,
                 col_v: str, col_p: str, left_label: str, left_ylabel: str,
                 title_metric: str, out_name: str, cpu_stat: str,
                 van_name: str, pro_name: str, left_step: float | None = None) -> None:
    """Trace UNE figure : métrique gauche (col_v/col_p) + CPU (moyen ou médian,
    selon cpu_stat) à droite.
    left_step : si défini, pas de graduation de l'axe GAUCHE (ex. 16 -> 0,16,32,…)."""
    if col_v not in m.columns or col_p not in m.columns:
        print(f"  [skip] {app}: colonne {col_v}/{col_p} absente")
        return

    # Colonne CPU : moyenne ou médiane. Fallback sur l'avg si la médiane n'est
    # pas présente (anciens CSV produits avant l'ajout de pi_cpu_busy_med_pct).
    cpu_col = f"pi_cpu_busy_{cpu_stat}_pct"
    if f"{cpu_col}_v" not in m.columns or f"{cpu_col}_p" not in m.columns:
        cpu_col = "pi_cpu_busy_avg_pct"
    cpu_word = {"pi_cpu_busy_avg_pct": "moyen",
                "pi_cpu_busy_med_pct": "médian",
                "pi_cpu_busy_q3_pct": "Q3"}.get(cpu_col, "moyen")

    fig, ax = plt.subplots(figsize=(max(9, 1.2 * len(rates)), 5.6))
    ax2 = ax.twinx()   # second axe Y (droite), partage le même axe X

    # métrique gauche : courbes pleines · CPU (droite) : courbes tiretées
    l1, = ax.plot(x, m[col_v], marker="o", linewidth=2, color=C_LEFT_VAN, label=f"{left_label} Vanilla")
    l2, = ax.plot(x, m[col_p], marker="o", linewidth=2, color=C_LEFT_PRO, label=f"{left_label} Prototype")
    l3, = ax2.plot(x, m[f"{cpu_col}_v"], marker="s", linestyle="--", linewidth=2, color=C_CPU_VAN, label="CPU Vanilla")
    l4, = ax2.plot(x, m[f"{cpu_col}_p"], marker="s", linestyle="--", linewidth=2, color=C_CPU_PRO, label="CPU Prototype")

    ax.set_xlabel("rate (Target Requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel(left_ylabel, fontsize=11, labelpad=8)
    ax2.set_ylabel(f"CPU {cpu_word} (%)", fontsize=11, labelpad=8)
    ax.set_xticks(x)
    ax.set_xticklabels(rates)
    ax.set_ylim(bottom=0)
    # Pas de graduation choisi pour l'axe GAUCHE (ex. RPS de 16 en 16).
    if left_step and left_step > 0:
        ax.yaxis.set_major_locator(MultipleLocator(left_step))
        vmax = float(np.nanmax([m[col_v].max(), m[col_p].max()]))
        top = (np.floor(vmax / left_step) + 1) * left_step   # multiple sup. du pas
        ax.set_ylim(0, top)
    ax2.set_ylim(0, CPU_YMAX)          # CPU : graduation TOUJOURS jusqu'à 100
    ax.grid(axis="y", alpha=0.2, linestyle="--")

    # légende commune (les 2 axes réunis)
    handles = [l1, l2, l3, l4]
    ax.legend(handles, [h.get_label() for h in handles],
              frameon=False, fontsize=9, ncol=2, loc="upper left")
    ax.set_title(f"{app} — {title_metric} & CPU {cpu_word} : Vanilla vs Prototype",
                 fontsize=13, fontweight="bold", pad=12)

    fig.tight_layout()
    out_dir = app_dir / f"rps_cpu_{scheme}"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / out_name
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"  [ok] {app}: {out_path}  (vanilla={van_name}, proto={pro_name})")


def plot_app(app_dir: Path, app: str, scheme: str, cpu_stat: str = "avg",
             van: Path | None = None, pro: Path | None = None,
             rps_step: float | None = None) -> None:
    # fichiers explicites (--vanilla/--proto) sinon auto-découverte
    van = van or find_csv(app_dir, "vanilla", scheme)
    pro = pro or find_csv(app_dir, "proto", scheme)
    if not van or not pro:
        print(f"  [skip] {app}: paire {scheme} introuvable (vanilla={bool(van)}, proto={bool(pro)})")
        return

    # outer join : on garde TOUS les rates présents dans l'un OU l'autre fichier
    # (aucun rate n'est sauté ; une valeur manquante d'un mode = point absent).
    m = pd.merge(prepare(van), prepare(pro), on="rate", suffixes=("_v", "_p"), how="outer").sort_values("rate")
    if m.empty:
        print(f"  [skip] {app}: aucun rate")
        return

    rates = m["rate"].astype(int).astype(str).to_list()
    x = np.arange(len(rates), dtype=float)

    # suffixe de nom de fichier : rien pour avg (défaut), _med pour la médiane
    # -> les deux versions coexistent sans s'écraser.
    sfx = "" if cpu_stat == "avg" else f"_{cpu_stat}"

    # Figure 1 : RPS atteint (gauche) + CPU (droite) — pas de graduation RPS = rps_step
    _plot_metric(app_dir, app, scheme, m, rates, x,
                 "rps_v", "rps_p", "RPS", "RPS atteint",
                 "RPS", f"{app}_{scheme}_rps_cpu{sfx}.png", cpu_stat, van.name, pro.name,
                 left_step=rps_step)

    # Figure 2 : latence moyenne (gauche) + CPU (droite)
    _plot_metric(app_dir, app, scheme, m, rates, x,
                 "lat_avg_ms_v", "lat_avg_ms_p", "Latence", "Latence moyenne (ms)",
                 "Latence moyenne", f"{app}_{scheme}_lat_cpu{sfx}.png", cpu_stat, van.name, pro.name)


def main() -> None:
    p = argparse.ArgumentParser(description="Latence & RPS (double axe, CPU 0-100) vs rate, Vanilla vs Prototype.")
    p.add_argument("--results-dir", default=str(Path(__file__).resolve().parent / "results"))
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--app", default=None, help="une seule application (def: toutes)")
    p.add_argument("--vanilla", default=None, help="chemin exact du CSV vanilla (override auto-découverte)")
    p.add_argument("--proto", default=None, help="chemin exact du CSV proto (override auto-découverte)")
    p.add_argument("--cpu-stat", choices=["avg", "med", "q3"], default="avg",
                   help="statistique CPU tracée à droite : avg (moyenne, défaut), med (médiane) ou q3 (3e quartile)")
    p.add_argument("--rps-step", type=float, default=None,
                   help="pas de graduation de l'axe RPS (ex. 16 -> 0,16,32,… ; défaut: auto)")
    args = p.parse_args()

    # Mode fichiers explicites : on trace juste cette paire.
    if args.vanilla and args.proto:
        van, pro = Path(args.vanilla).resolve(), Path(args.proto).resolve()
        app_dir = van.parent
        app = app_dir.name
        print(f"=== RPS/Latence + CPU[{args.cpu_stat}] [{args.scheme}] {app} (fichiers explicites) ===")
        plot_app(app_dir, app, args.scheme, cpu_stat=args.cpu_stat, van=van, pro=pro,
                 rps_step=args.rps_step)
        return

    root = Path(args.results_dir).expanduser().resolve()
    apps = [args.app] if args.app else APPS
    print(f"=== RPS/Latence + CPU[{args.cpu_stat}] [{args.scheme}] depuis {root} ===")
    for app in apps:
        app_dir = root / app
        if not app_dir.is_dir():
            print(f"  [skip] {app}: dossier absent")
            continue
        plot_app(app_dir, app, args.scheme, cpu_stat=args.cpu_stat, rps_step=args.rps_step)


if __name__ == "__main__":
    main()
