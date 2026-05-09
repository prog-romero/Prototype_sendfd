#!/usr/bin/env python3
"""
Grouped bar chart by concurrency (1-core): Vanilla vs Prototype throughput.

Usage:
  python3 scripts/plot_bar_1core_compare.py

Optional custom files:
  python3 scripts/plot_bar_1core_compare.py \
    --vanilla results/vanilla_http_alt_1core_32kb_v1.csv \
    --proto results/proto_http_alt_1core_32kb_v1.csv \
    --out results/compare_1core_throughput_bar.png

This script generates 3 figures:
    1) Throughput bars
    2) Average latency bars
    3) Total requests bars (with error counts printed above bars)
"""

import argparse
import os

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


def annotate_bars(ax, bars, fmt="{:.2f}", fontsize=6, dy=0.35):
    for bar in bars:
        h = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            h + dy,
            fmt.format(h),
            ha="center",
            va="bottom",
            fontsize=fontsize,
            rotation=90,
        )


def build_out_paths(base_out: str) -> tuple[str, str, str]:
    if base_out.endswith(".png"):
        stem = base_out[:-4]
    else:
        stem = base_out
    throughput_out = f"{stem}.png"
    latency_out = f"{stem}_latency_avg.png"
    requests_out = f"{stem}_requests.png"
    return throughput_out, latency_out, requests_out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--vanilla",
        default="results/vanilla_http_alt_1core_32kb_v1.csv",
        help="Path to vanilla 1-core CSV",
    )
    parser.add_argument(
        "--proto",
        default="results/proto_http_alt_1core_32kb_v1.csv",
        help="Path to prototype 1-core CSV",
    )
    parser.add_argument(
        "--out",
        default="results/compare_1core_throughput_bar.png",
        help="Output PNG path",
    )
    args = parser.parse_args()

    vanilla = pd.read_csv(args.vanilla)
    proto = pd.read_csv(args.proto)

    # Keep only common concurrency points for a clean side-by-side comparison.
    common = sorted(set(vanilla["concurrency"]).intersection(set(proto["concurrency"])))
    vanilla = vanilla[vanilla["concurrency"].isin(common)].sort_values("concurrency")
    proto = proto[proto["concurrency"].isin(common)].sort_values("concurrency")

    x = np.arange(len(common))
    labels = [str(c) for c in common]
    width = 0.38

    throughput_out, latency_out, requests_out = build_out_paths(args.out)

    fig, ax = plt.subplots(figsize=(13, 5.5))

    bars_v = ax.bar(
        x - width / 2,
        vanilla["transfer_kb_s"],
        width=width,
        label="Vanilla",
        color="#1f77b4",
    )
    bars_p = ax.bar(
        x + width / 2,
        proto["transfer_kb_s"],
        width=width,
        label="Prototype",
        color="#ff7f0e",
    )

    annotate_bars(ax, bars_v, fmt="{:.2f}", fontsize=6, dy=0.35)
    annotate_bars(ax, bars_p, fmt="{:.2f}", fontsize=6, dy=0.35)

    ax.set_title("Throughput Comparison (1 Core)")
    ax.set_xlabel("Concurrency")
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45)
    ax.legend()
    ax.grid(axis="y", alpha=0.25)

    plt.tight_layout()
    out_dir = os.path.dirname(throughput_out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    fig.savefig(throughput_out, dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(13, 5.5))

    bars_v = ax.bar(
        x - width / 2,
        vanilla["lat_avg_ms"],
        width=width,
        label="Vanilla",
        color="#1f77b4",
    )
    bars_p = ax.bar(
        x + width / 2,
        proto["lat_avg_ms"],
        width=width,
        label="Prototype",
        color="#ff7f0e",
    )

    annotate_bars(ax, bars_v, fmt="{:.1f}", fontsize=6, dy=2.0)
    annotate_bars(ax, bars_p, fmt="{:.1f}", fontsize=6, dy=2.0)

    ax.set_title("Average Latency Comparison (1 Core)")
    ax.set_xlabel("Concurrency")
    ax.set_ylabel("Average latency (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45)
    ax.legend()
    ax.grid(axis="y", alpha=0.25)

    plt.tight_layout()
    fig.savefig(latency_out, dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(13, 5.5))

    bars_v = ax.bar(
        x - width / 2,
        vanilla["total_requests"],
        width=width,
        label="Vanilla",
        color="#1f77b4",
    )
    bars_p = ax.bar(
        x + width / 2,
        proto["total_requests"],
        width=width,
        label="Prototype",
        color="#ff7f0e",
    )

    # Show error counts above each total-requests bar.
    for bar, err in zip(bars_v, vanilla["errors_non2xx"]):
        x0 = bar.get_x() + bar.get_width() / 2
        y0 = bar.get_height() + 75
        ax.text(x0, y0, f"e:{int(err)}", fontsize=6, color="#1f77b4", ha="center", va="bottom")

    for bar, err in zip(bars_p, proto["errors_non2xx"]):
        x0 = bar.get_x() + bar.get_width() / 2
        y0 = bar.get_height() + 75
        ax.text(x0, y0, f"e:{int(err)}", fontsize=6, color="#ff7f0e", ha="center", va="bottom")

    ax.set_title("Total Requests Comparison (1 Core)")
    ax.set_xlabel("Concurrency")
    ax.set_ylabel("Total requests in step")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45)
    ax.legend()
    ax.grid(axis="y", alpha=0.25)

    plt.tight_layout()
    fig.savefig(requests_out, dpi=180)
    plt.close(fig)

    print(f"Saved: {throughput_out}")
    print(f"Saved: {latency_out}")
    print(f"Saved: {requests_out}")


if __name__ == "__main__":
    main()
