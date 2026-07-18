#!/usr/bin/env python3
"""
boxplot_sebs_compare.py — Boîtes à moustache Vanilla vs Prototype par application.

Jumeau de plot_sebs_compare.py, mais en BOÎTES À MOUSTACHE au lieu d'histogrammes.
Pour chaque métrique d'une application : UNE figure avec DEUX boîtes (Vanilla,
Prototype). Chaque boîte résume la distribution de la métrique sur toute la
PLAGE DE RPS balayée (2→256) — une valeur par palier. On voit ainsi médiane,
quartiles et étendue de la métrique sur le sweep, côte à côte.

Sortie : results/<app>/boxplot_<scheme>/  (un dossier boxplot par application).
Sur l'overhead, les valeurs négatives ne sont PAS prises en compte.

Usage :
  python3 boxplot_sebs_compare.py --scheme https                 # toutes les apps
  python3 boxplot_sebs_compare.py --scheme https --app compression
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

# (colonne, nom_fichier, libellé y, masquer_négatifs)
BOX_SPECS = [
    ("rps",                  "rps",             "requests/s",        False),
    ("net_kb_s_avg",         "net_kb_s",        "réseau moy (kB/s)", False),
    ("pi_cpu_busy_avg_pct",  "cpu_avg_pct",     "cpu moy (%)",       False),
    ("pi_cpu_busy_max_pct",  "cpu_max_pct",     "cpu max (%)",       False),
    ("lat_avg_ms",           "lat_avg_ms",      "latence moy (ms)",  False),
    ("client_ms_avg",        "client_ms_avg",   "client moy (ms)",   False),
    ("client_ms_p99",        "client_ms_p99",   "client p99 (ms)",   False),
    ("server_ms_avg",        "server_ms_avg",   "serveur moy (ms)",  False),
    ("server_ms_p99",        "server_ms_p99",   "serveur p99 (ms)",  False),
    ("overhead_ms_avg",      "overhead_ms_avg", "overhead moy (ms)", True),
    ("overhead_ms_p99",      "overhead_ms_p99", "overhead p99 (ms)", True),
]

COLOR_A = "#1f77b4"   # Vanilla
COLOR_B = "#d95f02"   # Prototype


def find_csv(app_dir: Path, mode: str, scheme: str) -> Path | None:
    cands = sorted(app_dir.glob(f"{mode}_{scheme}*.csv"), reverse=True)
    for c in cands:
        try:
            cols = pd.read_csv(c, nrows=0).columns
        except Exception:
            continue
        if "overhead_ms_avg" in cols:
            return c
    return cands[0] if cands else None


def prepare(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    return df.sort_values("rate").drop_duplicates(subset=["rate"], keep="last")


def _values(df: pd.DataFrame, col: str, mask_neg: bool):
    if col not in df.columns:
        return np.array([])
    v = pd.to_numeric(df[col], errors="coerce").to_numpy(dtype=float)
    v = v[~np.isnan(v)]
    if mask_neg:
        v = v[v >= 0]
    return v


def make_box(df_a, df_b, col, ylabel, out_path, label_a, label_b, app, rate_lo, rate_hi, mask_neg):
    a = _values(df_a, col, mask_neg)
    b = _values(df_b, col, mask_neg)
    if a.size == 0 and b.size == 0:
        return False

    fig, ax = plt.subplots(figsize=(7.5, 5.2))
    bp = ax.boxplot([a, b], positions=[1, 2], widths=0.5, patch_artist=True,
                    showmeans=True, meanline=True,
                    medianprops=dict(color="black", linewidth=1.6),
                    meanprops=dict(color="crimson", linestyle="--", linewidth=1.4),
                    flierprops=dict(marker="o", markersize=4, alpha=0.5))
    for patch, color in zip(bp["boxes"], (COLOR_A, COLOR_B)):
        patch.set_facecolor(color)
        patch.set_alpha(0.55)

    # points bruts (1 par palier de RPS) superposés pour voir la dispersion réelle
    for xpos, vals, color in ((1, a, COLOR_A), (2, b, COLOR_B)):
        if vals.size:
            jitter = np.random.uniform(-0.06, 0.06, size=vals.size)
            ax.scatter(np.full(vals.size, xpos) + jitter, vals, s=18,
                       color=color, edgecolor="black", linewidth=0.3, zorder=3, alpha=0.8)

    ax.set_xticks([1, 2])
    ax.set_xticklabels([label_a, label_b])
    ax.set_ylabel(ylabel, fontsize=11, labelpad=8)
    ax.set_xlabel(f"distribution sur la plage RPS {rate_lo}→{rate_hi}", fontsize=10, labelpad=8)
    ax.grid(axis="y", alpha=0.2, linestyle="--")
    ax.set_title(f"{app} — Vanilla vs Prototype : {ylabel}", fontsize=12, fontweight="bold", pad=12)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def boxplot_app(app_dir: Path, app: str, scheme: str, label_a: str, label_b: str) -> None:
    van = find_csv(app_dir, "vanilla", scheme)
    pro = find_csv(app_dir, "proto", scheme)
    if not van or not pro:
        print(f"  [skip] {app}: paire {scheme} introuvable (vanilla={bool(van)}, proto={bool(pro)})")
        return

    df_a, df_b = prepare(van), prepare(pro)
    rates = sorted(set(df_a["rate"]).union(df_b["rate"]))
    rate_lo, rate_hi = (int(rates[0]), int(rates[-1])) if rates else (0, 0)

    out_dir = app_dir / f"boxplot_{scheme}"
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for col, stem, ylabel, mask_neg in BOX_SPECS:
        if make_box(df_a, df_b, col, ylabel, out_dir / f"{app}_{scheme}_box_{stem}.png",
                    label_a, label_b, app, rate_lo, rate_hi, mask_neg):
            n += 1
    print(f"  [ok] {app}: {n} boîtes -> {out_dir}  (vanilla={van.name}, proto={pro.name})")


def main() -> None:
    p = argparse.ArgumentParser(description="Boîtes à moustache Vanilla vs Prototype par application SeBS.")
    p.add_argument("--results-dir", default=str(Path(__file__).resolve().parent / "results"))
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--app", default=None, help="une seule application (def: toutes)")
    p.add_argument("--label-a", default="Vanilla")
    p.add_argument("--label-b", default="Prototype")
    args = p.parse_args()

    root = Path(args.results_dir).expanduser().resolve()
    apps = [args.app] if args.app else APPS
    print(f"=== boxplots SeBS [{args.scheme}] depuis {root} ===")
    for app in apps:
        app_dir = root / app
        if not app_dir.is_dir():
            print(f"  [skip] {app}: dossier absent")
            continue
        boxplot_app(app_dir, app, args.scheme, args.label_a, args.label_b)


if __name__ == "__main__":
    main()
