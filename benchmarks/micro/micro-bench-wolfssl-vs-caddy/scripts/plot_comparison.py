#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


REQUIRED_COLUMNS = {
    "implementation",
    "payload_size",
    "mean_ns",
    "p50_ns",
    "p99_ns",
}

IMPLEMENTATION_ORDER = ["wolfssl", "caddy"]
IMPLEMENTATION_STYLE = {
    "wolfssl": {"label": "wolfSSL", "color": "#1f77b4"},
    "caddy": {"label": "Caddy", "color": "#d62728"},
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot wolfSSL vs Caddy benchmark comparison from a summary CSV."
    )
    parser.add_argument("--in", dest="input_csv", required=True, help="Path to summary CSV")
    parser.add_argument("--out", dest="output_png", help="Path to output PNG")
    parser.add_argument(
        "--title",
        default="wolfSSL vs Caddy request read/decrypt comparison",
        help="Figure title",
    )
    return parser.parse_args()


def validate_columns(df: pd.DataFrame) -> None:
    missing = REQUIRED_COLUMNS.difference(df.columns)
    if missing:
        missing_text = ", ".join(sorted(missing))
        raise ValueError(f"missing required columns: {missing_text}")


def implementation_rows(df: pd.DataFrame, implementation: str) -> pd.DataFrame:
    rows = df[df["implementation"].str.lower() == implementation].copy()
    return rows.sort_values("payload_size")


def plot_metric(ax: plt.Axes, df: pd.DataFrame, metric: str, ylabel: str) -> None:
    for implementation in IMPLEMENTATION_ORDER:
        rows = implementation_rows(df, implementation)
        if rows.empty:
            continue

        style = IMPLEMENTATION_STYLE[implementation]
        ax.plot(
            rows["payload_size"],
            rows[metric] / 1000.0,
            marker="o",
            linewidth=2,
            markersize=6,
            color=style["color"],
            label=style["label"],
        )

    ax.set_xscale("log", base=2)
    ax.set_xlabel("Payload size (bytes)")
    ax.set_ylabel(ylabel)
    ax.grid(True, which="both", linestyle=":", linewidth=0.8, alpha=0.7)
    ax.legend()


def main() -> int:
    args = parse_args()

    input_csv = Path(args.input_csv)
    output_png = Path(args.output_png) if args.output_png else input_csv.with_suffix(".png")

    df = pd.read_csv(input_csv)
    validate_columns(df)

    numeric_columns = ["payload_size", "mean_ns", "p50_ns", "p99_ns"]
    for column in numeric_columns:
        df[column] = pd.to_numeric(df[column], errors="raise")

    fig, axes = plt.subplots(2, 1, figsize=(10, 9), sharex=True)
    fig.suptitle(args.title, fontsize=14)

    plot_metric(axes[0], df, "mean_ns", "Mean latency (us)")
    axes[0].set_title("Mean latency")

    plot_metric(axes[1], df, "p99_ns", "P99 latency (us)")
    axes[1].set_title("P99 latency")

    plt.tight_layout()
    fig.subplots_adjust(top=0.92)

    output_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_png, dpi=200)
    plt.close(fig)

    print(f"wrote comparison plot to {output_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())