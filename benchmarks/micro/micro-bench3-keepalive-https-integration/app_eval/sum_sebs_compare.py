#!/usr/bin/env python3
"""
sum_sebs_compare.py — Histogramme des SOMMES Vanilla vs Prototype par application.

Pour chaque métrique d'une application : UNE figure avec DEUX barres
(Vanilla, Prototype). La hauteur de chaque barre = la SOMME de toutes les valeurs
de la métrique sur la PLAGE DE RPS balayée (2→256), soit une valeur par palier
additionnée. En abscisse : la plage de RPS ; en ordonnée : la somme.

Sortie : results/<app>/sum_<scheme>/  (un dossier par application).
Sur l'overhead, les valeurs négatives ne sont PAS comptées dans la somme.

Usage :
  python3 sum_sebs_compare.py --scheme https                 # toutes les apps
  python3 sum_sebs_compare.py --scheme https --app compression
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
SUM_SPECS = [
    ("rps",                  "rps",             "somme requests/s",    False),
    ("net_kb_s_avg",         "net_kb_s",        "somme réseau (kB/s)", False),
    ("pi_cpu_busy_avg_pct",  "cpu_avg_pct",     "somme cpu moy (%)",   False),
    ("pi_cpu_busy_max_pct",  "cpu_max_pct",     "somme cpu max (%)",   False),
    ("lat_avg_ms",           "lat_avg_ms",      "somme latence (ms)",  False),
    ("client_ms_avg",        "client_ms_avg",   "somme client (ms)",   False),
    ("client_ms_p99",        "client_ms_p99",   "somme client p99 (ms)", False),
    ("server_ms_avg",        "server_ms_avg",   "somme serveur (ms)",  False),
    ("server_ms_p99",        "server_ms_p99",   "somme serveur p99 (ms)", False),
    ("overhead_ms_avg",      "overhead_ms_avg", "somme overhead (ms)", True),
    ("overhead_ms_p99",      "overhead_ms_p99", "somme overhead p99 (ms)", True),
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


def _sum(df: pd.DataFrame, col: str, mask_neg: bool) -> float | None:
    if col not in df.columns:
        return None
    v = pd.to_numeric(df[col], errors="coerce").to_numpy(dtype=float)
    v = v[~np.isnan(v)]
    if mask_neg:
        v = v[v >= 0]
    return float(v.sum()) if v.size else 0.0


def make_sum_bar(df_a, df_b, col, ylabel, out_path, label_a, label_b, app, rate_lo, rate_hi, mask_neg):
    a = _sum(df_a, col, mask_neg)
    b = _sum(df_b, col, mask_neg)
    if a is None and b is None:
        return False
    a = a or 0.0
    b = b or 0.0

    fig, ax = plt.subplots(figsize=(7.0, 5.0))
    bars = ax.bar([1, 2], [a, b], width=0.55, color=[COLOR_A, COLOR_B], alpha=0.75,
                  edgecolor="black", linewidth=0.5)
    for bar, val in zip(bars, (a, b)):
        txt = f"{val:.0f}" if abs(val) >= 100 else f"{val:.2f}"
        ax.text(bar.get_x() + bar.get_width() / 2.0, bar.get_height(), txt,
                ha="center", va="bottom", fontsize=10, fontweight="bold")

    ax.set_xticks([1, 2])
    ax.set_xticklabels([label_a, label_b])
    ax.set_ylabel(ylabel, fontsize=11, labelpad=8)
    ax.set_xlabel(f"somme sur la plage RPS {rate_lo}→{rate_hi}", fontsize=10, labelpad=8)
    ax.grid(axis="y", alpha=0.2, linestyle="--")
    ax.margins(y=0.15)
    ax.set_title(f"{app} — Vanilla vs Prototype : {ylabel}", fontsize=12, fontweight="bold", pad=12)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def sum_app(app_dir: Path, app: str, scheme: str, label_a: str, label_b: str) -> None:
    van = find_csv(app_dir, "vanilla", scheme)
    pro = find_csv(app_dir, "proto", scheme)
    if not van or not pro:
        print(f"  [skip] {app}: paire {scheme} introuvable (vanilla={bool(van)}, proto={bool(pro)})")
        return

    df_a, df_b = prepare(van), prepare(pro)
    rates = sorted(set(df_a["rate"]).union(df_b["rate"]))
    rate_lo, rate_hi = (int(rates[0]), int(rates[-1])) if rates else (0, 0)

    out_dir = app_dir / f"sum_{scheme}"
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for col, stem, ylabel, mask_neg in SUM_SPECS:
        if make_sum_bar(df_a, df_b, col, ylabel, out_dir / f"{app}_{scheme}_sum_{stem}.png",
                        label_a, label_b, app, rate_lo, rate_hi, mask_neg):
            n += 1
    print(f"  [ok] {app}: {n} histogrammes -> {out_dir}  (vanilla={van.name}, proto={pro.name})")


def main() -> None:
    p = argparse.ArgumentParser(description="Histogramme des sommes Vanilla vs Prototype par application SeBS.")
    p.add_argument("--results-dir", default=str(Path(__file__).resolve().parent / "results"))
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--app", default=None, help="une seule application (def: toutes)")
    p.add_argument("--label-a", default="Vanilla")
    p.add_argument("--label-b", default="Prototype")
    args = p.parse_args()

    root = Path(args.results_dir).expanduser().resolve()
    apps = [args.app] if args.app else APPS
    print(f"=== histogrammes des sommes SeBS [{args.scheme}] depuis {root} ===")
    for app in apps:
        app_dir = root / app
        if not app_dir.is_dir():
            print(f"  [skip] {app}: dossier absent")
            continue
        sum_app(app_dir, app, args.scheme, args.label_a, args.label_b)


if __name__ == "__main__":
    main()
