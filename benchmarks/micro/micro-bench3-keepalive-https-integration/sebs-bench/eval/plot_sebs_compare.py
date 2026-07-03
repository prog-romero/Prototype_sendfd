#!/usr/bin/env python3
"""
plot_sebs_compare.py — Plots Vanilla vs Prototype par application SeBS.

Reprend le style de evaluation_throughput/compare_two_csv_plots.py (barres
groupées vanilla/proto + ligne + annotation des timeouts), et AJOUTE nos
nouvelles métriques perf-cost : client_ms, server_ms, overhead_ms.

Pour chaque application, cherche dans results/<app>/ la paire de CSV
(vanilla_<scheme>*.csv, proto_<scheme>*.csv) au NOUVEAU format (colonnes
overhead_ms_*), et écrit toutes les figures dans results/<app>/plots_<scheme>/.

Sur les plots d'OVERHEAD, les valeurs négatives ne sont PAS affichées (mises à
NaN) — un overhead négatif n'a pas de sens physique (artefact d'agrégat).

Usage :
  # toutes les applications trouvées dans results/ :
  python3 plot_sebs_compare.py --scheme https
  # une seule application :
  python3 plot_sebs_compare.py --scheme https --app dynamic-html
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

# (colonne_source, nom_fichier, libellé y, masquer_négatifs)
PLOT_SPECS = [
    # — les 6 plots "à la evaluation_throughput" —
    ("rps",                  "rps",             "requests/s",           False),
    ("net_kb_s_avg",         "net_kb_s",        "réseau moy (kB/s)",    False),
    ("pi_cpu_busy_avg_pct",  "cpu_avg_pct",     "cpu moy (%)",          False),
    ("pi_cpu_busy_max_pct",  "cpu_max_pct",     "cpu max (%)",          False),
    ("lat_avg_ms",           "lat_avg_ms",      "latence moy (ms)",     False),
    ("total_requests",       "total_requests",  "requêtes totales",     False),
    # — nos nouvelles métriques perf-cost —
    ("client_ms_avg",        "client_ms_avg",   "client moy (ms)",      False),
    ("client_ms_p99",        "client_ms_p99",   "client p99 (ms)",      False),
    ("server_ms_avg",        "server_ms_avg",   "serveur moy (ms)",     False),
    ("server_ms_p99",        "server_ms_p99",   "serveur p99 (ms)",     False),
    ("overhead_ms_avg",      "overhead_ms_avg", "overhead moy (ms)",    True),
    ("overhead_ms_p50",      "overhead_ms_p50", "overhead p50 (ms)",    True),
    ("overhead_ms_p99",      "overhead_ms_p99", "overhead p99 (ms)",    True),
]


def find_csv(app_dir: Path, mode: str, scheme: str) -> Path | None:
    """Choisit le CSV <mode>_<scheme>*.csv au nouveau format (avec overhead_ms_avg),
    en préférant le suffixe _1 (le plus récent)."""
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
    df = df.sort_values("rate").drop_duplicates(subset=["rate"], keep="last")
    if "socket_timeout_errors" not in df.columns:
        df["socket_timeout_errors"] = 0
    return df


def _annotate_timeouts(ax, bars, timeouts) -> None:
    for bar, t in zip(bars, timeouts):
        if pd.notna(t) and int(t) > 0:
            ax.text(bar.get_x() + bar.get_width() / 2.0, bar.get_height(),
                    str(int(t)), ha="center", va="bottom", fontsize=8, color="crimson")


def make_plot(merged, metric, ylabel, out_path, label_a, label_b, app, mask_neg):
    if f"{metric}_a" not in merged.columns or f"{metric}_b" not in merged.columns:
        return False
    x_labels = merged["rate"].astype(int).astype(str).to_list()
    x = np.arange(len(x_labels), dtype=float)
    width = 0.36

    a = merged[f"{metric}_a"].astype(float).to_numpy()
    b = merged[f"{metric}_b"].astype(float).to_numpy()
    if mask_neg:                       # overhead : on n'affiche pas le négatif
        a = np.where(a < 0, np.nan, a)
        b = np.where(b < 0, np.nan, b)
    if np.all(np.isnan(a)) and np.all(np.isnan(b)):
        return False

    a_tmo = merged.get("socket_timeout_errors_a", pd.Series([0] * len(x))).fillna(0)
    b_tmo = merged.get("socket_timeout_errors_b", pd.Series([0] * len(x))).fillna(0)

    fig, ax = plt.subplots(figsize=(10, 4.8))
    bars_a = ax.bar(x - width / 2, np.nan_to_num(a), width=width, alpha=0.65, label=label_a, color="#1f77b4")
    bars_b = ax.bar(x + width / 2, np.nan_to_num(b), width=width, alpha=0.65, label=label_b, color="#d95f02")
    ax.plot(x - width / 2, a, marker="o", linewidth=1.5, color="#1f77b4")
    ax.plot(x + width / 2, b, marker="o", linewidth=1.5, color="#d95f02")

    _annotate_timeouts(ax, bars_a, a_tmo.to_list())
    _annotate_timeouts(ax, bars_b, b_tmo.to_list())

    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, rotation=45, ha="right")
    ax.set_xlabel("rate (Target Requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel(ylabel, fontsize=11, labelpad=8)
    ax.grid(axis="y", alpha=0.2, linestyle="--")
    ax.legend(frameon=False, fontsize=10)
    ax.set_title(f"{app} — Vanilla vs Prototype : {ylabel}", fontsize=13, fontweight="bold", pad=12)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def plot_app(app_dir: Path, app: str, scheme: str, label_a: str, label_b: str) -> None:
    van = find_csv(app_dir, "vanilla", scheme)
    pro = find_csv(app_dir, "proto", scheme)
    if not van or not pro:
        print(f"  [skip] {app}: paire {scheme} introuvable (vanilla={bool(van)}, proto={bool(pro)})")
        return

    merged = pd.merge(prepare(van), prepare(pro), on="rate", suffixes=("_a", "_b"), how="inner")
    if merged.empty:
        print(f"  [skip] {app}: aucun rate commun entre {van.name} et {pro.name}")
        return

    out_dir = app_dir / f"plots_{scheme}"
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for metric, stem, ylabel, mask_neg in PLOT_SPECS:
        if make_plot(merged, metric, ylabel, out_dir / f"{app}_{scheme}_{stem}.png",
                     label_a, label_b, app, mask_neg):
            n += 1
    print(f"  [ok] {app}: {n} figures -> {out_dir}  (vanilla={van.name}, proto={pro.name})")


def main() -> None:
    p = argparse.ArgumentParser(description="Plots Vanilla vs Prototype par application SeBS.")
    p.add_argument("--results-dir", default=str(Path(__file__).resolve().parent / "results"),
                   help="dossier results/ (def: ./results)")
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--app", default=None, help="une seule application (def: toutes)")
    p.add_argument("--label-a", default="Vanilla")
    p.add_argument("--label-b", default="Prototype")
    args = p.parse_args()

    root = Path(args.results_dir).expanduser().resolve()
    apps = [args.app] if args.app else APPS
    print(f"=== plots SeBS [{args.scheme}] depuis {root} ===")
    for app in apps:
        app_dir = root / app
        if not app_dir.is_dir():
            print(f"  [skip] {app}: dossier absent")
            continue
        plot_app(app_dir, app, args.scheme, args.label_a, args.label_b)


if __name__ == "__main__":
    main()
