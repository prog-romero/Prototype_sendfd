#!/usr/bin/env python3
"""
compare_two_csv_plots.py — Compare two wrk2 rate sweep CSV files and create 5 simple figures.

Usage:
    python3 compare_two_csv_plots.py <csv_vanilla> <csv_proto> --prefix bench_max --out-dir plots/compare_figures_32kb
"""

import argparse
from pathlib import Path
import sys

try:
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
except ImportError:
    print("ERROR: matplotlib, numpy, and pandas are required. Install with: pip3 install matplotlib numpy pandas", file=sys.stderr)
    sys.exit(1)


REQUIRED_COLUMNS = [
    "rate",
    "rps",
    "transfer_kb_s",
    "pi_cpu_busy_avg_pct",
    "lat_avg_ms",
    "total_requests",
    "socket_timeout_errors",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare two benchmark CSV files and create 5 simple figures."
    )
    parser.add_argument("csv_a", help="Path to first CSV (ex: vanilla)")
    parser.add_argument("csv_b", help="Path to second CSV (ex: prototype)")
    parser.add_argument("--label-a", default="Vanilla", help="Legend label for first CSV")
    parser.add_argument("--label-b", default="Prototype", help="Legend label for second CSV")
    parser.add_argument(
        "--out-dir",
        default="plots/compare_figures_32kb",
        help="Directory for generated figures",
    )
    parser.add_argument(
        "--prefix",
        default="bench_max",
        help="Filename prefix for generated figures",
    )
    return parser.parse_args()


def load_csv(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    missing = [c for c in REQUIRED_COLUMNS if c not in df.columns]
    if missing:
        raise ValueError(f"Missing required columns in {path}: {', '.join(missing)}")
    return df.copy()


def prepare(df: pd.DataFrame) -> pd.DataFrame:
    out = df[[
        "rate",
        "rps",
        "transfer_kb_s",
        "pi_cpu_busy_avg_pct",
        "lat_avg_ms",
        "total_requests",
        "socket_timeout_errors",
    ]].copy()
    out = out.sort_values("rate").drop_duplicates(subset=["rate"], keep="last")
    return out


def annotate_timeouts(ax, bars, timeout_series: pd.Series) -> None:
    for bar, t in zip(bars, timeout_series.to_list()):
        if pd.notna(t) and int(t) > 0:
            ax.text(
                bar.get_x() + bar.get_width() / 2.0,
                bar.get_height(),
                str(int(t)),
                ha="center",
                va="bottom",
                fontsize=8,
            )


def make_plot(
    merged: pd.DataFrame,
    metric: str,
    y_label: str,
    out_path: Path,
    label_a: str,
    label_b: str,
) -> None:
    x_labels = merged["rate"].astype(int).astype(str).to_list()
    x = np.arange(len(x_labels), dtype=float)
    width = 0.36

    a_vals = merged[f"{metric}_a"].to_numpy()
    b_vals = merged[f"{metric}_b"].to_numpy()

    a_tmo = merged["socket_timeout_errors_a"].fillna(0)
    b_tmo = merged["socket_timeout_errors_b"].fillna(0)

    fig, ax = plt.subplots(figsize=(10, 4.8))

    bars_a = ax.bar(x - width / 2, a_vals, width=width, alpha=0.65, label=label_a, color="#1f77b4")
    bars_b = ax.bar(x + width / 2, b_vals, width=width, alpha=0.65, label=label_b, color="#d95f02")

    ax.plot(x - width / 2, a_vals, marker="o", linewidth=1.5, color="#1f77b4")
    ax.plot(x + width / 2, b_vals, marker="o", linewidth=1.5, color="#d95f02")

    annotate_timeouts(ax, bars_a, a_tmo)
    annotate_timeouts(ax, bars_b, b_tmo)

    ax.set_xticks(x)
    ax.set_xticklabels(x_labels)
    ax.set_xlabel("rate (Target Requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel(y_label, fontsize=11, labelpad=8)
    ax.grid(axis="y", alpha=0.2, linestyle="--")
    ax.legend(frameon=False, fontsize=10)
    ax.set_title(f"Vanilla vs SCM_RIGHTS Prototype: {y_label.capitalize()}", fontsize=13, fontweight="bold", pad=12)

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def main() -> None:
    args = parse_args()

    df_a = prepare(load_csv(args.csv_a))
    df_b = prepare(load_csv(args.csv_b))

    merged = pd.merge(df_a, df_b, on="rate", suffixes=("_a", "_b"), how="inner")
    if merged.empty:
        raise ValueError("No common rate values found between the two CSV files.")

    out_dir = Path(args.out_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    plot_specs = [
        ("transfer_kb_s", "throughput_kb_s", "throughput (KB/s)"),
        ("rps", "rps", "requests/s"),
        ("pi_cpu_busy_avg_pct", "cpu_avg_pct", "cpu avg (%)"),
        ("lat_avg_ms", "lat_avg_ms", "latency avg (ms)"),
        ("total_requests", "total_requests", "total requests"),
    ]

    for metric, stem, ylabel in plot_specs:
        out_path = out_dir / f"{args.prefix}_{stem}.png"
        make_plot(
            merged=merged,
            metric=metric,
            y_label=ylabel,
            out_path=out_path,
            label_a=args.label_a,
            label_b=args.label_b,
        )

    print(f"[ok] Wrote 5 figures in: {out_dir}")


if __name__ == "__main__":
    main()
