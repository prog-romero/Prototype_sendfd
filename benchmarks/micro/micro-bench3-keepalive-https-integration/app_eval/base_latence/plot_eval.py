#!/usr/bin/env python3
"""
Plot latency vs payload size from run_eval.py CSV output.

For each mode: one bar per payload size (avg latency reported by wrk2),
with a std-dev error bar, and a curve connecting the bar tops.

Usage
-----
  # Proto + vanilla on the same chart:
  python3 plot_eval.py results/proto.csv results/vanilla.csv \\
      --output plots/latency_vs_payload.png

  # Single mode:
  python3 plot_eval.py results/proto.csv --output plots/proto.png

  # Choose a different latency metric (avg_ms, p50_ms, p75_ms, p90_ms, p99_ms):
  python3 plot_eval.py results/proto.csv results/vanilla.csv \\
      --metric p99_ms --output plots/p99_vs_payload.png
"""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

_COLORS = {
    "proto":   "#2196F3",   # blue
    "vanilla": "#FF9800",   # orange
}
_LABELS = {
    "proto":   "Prototype ",
    "vanilla": "Vanilla ",
}


def load_csv(path: str) -> list[dict]:
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({
                "mode":     row["mode"],
                "size_kb":  int(row["size_kb"]),
                "avg_ms":   float(row["avg_ms"]),
                "stdev_ms": float(row["stdev_ms"]),
                "p50_ms":   float(row["p50_ms"]),
                "p75_ms":   float(row["p75_ms"]),
                "p90_ms":   float(row["p90_ms"]),
                "p99_ms":   float(row["p99_ms"]),
            })
    return sorted(rows, key=lambda r: r["size_kb"])


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot latency vs payload size.")
    parser.add_argument("csvfiles", nargs="+", help="CSV files from run_eval.py")
    parser.add_argument("--output", default="latency_vs_payload.png")
    parser.add_argument(
        "--metric", default="avg_ms",
        choices=["avg_ms", "p50_ms", "p75_ms", "p90_ms", "p99_ms"],
        help="Latency metric to plot (default: avg_ms)",
    )
    parser.add_argument("--title", default=None)
    args = parser.parse_args()

    metric_label = {
        "avg_ms": "Mean latency (ms)",
        "p50_ms": "p50 latency (ms)",
        "p75_ms": "p75 latency (ms)",
        "p90_ms": "p90 latency (ms)",
        "p99_ms": "p99 latency (ms)",
    }[args.metric]

    datasets = [load_csv(p) for p in args.csvfiles]
    all_sizes = sorted(set(r["size_kb"] for d in datasets for r in d))
    x = np.arange(len(all_sizes))

    n = len(datasets)
    bar_w = 0.6 / max(n, 1)

    fig, ax = plt.subplots(figsize=(max(8, len(all_sizes) * 1.4 + 2), 6))

    for i, data in enumerate(datasets):
        mode  = data[0]["mode"]
        color = _COLORS.get(mode, f"C{i}")
        label = _LABELS.get(mode, mode)

        by_size = {r["size_kb"]: r for r in data}
        vals  = np.array([by_size[s][args.metric]  if s in by_size else np.nan for s in all_sizes])
        stdev = np.array([by_size[s]["stdev_ms"]   if s in by_size else np.nan for s in all_sizes])

        offset = (i - (n - 1) / 2) * bar_w
        xpos   = x + offset

        # Bars
        ax.bar(xpos, vals, bar_w,
               color=color, alpha=0.78, zorder=2, label=label)

        # Std-dev error bars
        ax.errorbar(xpos, vals, yerr=stdev,
                    fmt="none", color="black",
                    capsize=5, capthick=1.2, linewidth=1.2, zorder=3)

        # Curve connecting bar tops
        valid = ~np.isnan(vals)
        ax.plot(xpos[valid], vals[valid], "o-",
                color=color, linewidth=1.8, markersize=6, zorder=4)

        # Value labels above each bar
        for xp, v in zip(xpos, vals):
            if not np.isnan(v):
                ax.text(xp, v + 0.3, f"{v:.1f}",
                        ha="center", va="bottom", fontsize=7.5)

    title = args.title or (
        f"{metric_label} vs Images size\n"
        "(one TCP+TLS connection per request, no keepalive, wrk2 -c1)"
    )
    ax.set_xlabel("Images size (KB)", fontsize=12)
    ax.set_ylabel(metric_label, fontsize=12)
    ax.set_title(title, fontsize=11)
    ax.set_xticks(x)
    ax.set_xticklabels([f"{s} KB" for s in all_sizes])
    ax.set_ylim(bottom=0)
    ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
    ax.grid(axis="y", which="major", alpha=0.35, zorder=0)
    ax.grid(axis="y", which="minor", alpha=0.15, linestyle=":", zorder=0)
    ax.legend(fontsize=10)

    plt.tight_layout()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(args.output, dpi=150)
    print(f"Saved → {args.output}")
    plt.show()


if __name__ == "__main__":
    main()
