#!/usr/bin/env python3
"""
plot_vanilla_vs_proto.py — Plot comparison of Vanilla vs Prototype gateway latencies

Usage:
    python3 benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/plot_vanilla_vs_proto.py \
        --vanilla benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/results/vanilla_results_custom_32_to_512_step32.csv \
        --proto benchmarks/micro/micro-bench3-keepalive-http-integration/evaluation/results/proto_results_custom_32_to_512_step32.csv
"""

import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required. Install with: pip3 install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


def ns_to_ms(value: float) -> float:
    return value / 1_000_000.0


def load_delta_by_payload(csv_path: Path) -> dict[int, list[float]]:
    grouped: dict[int, list[float]] = defaultdict(list)
    with csv_path.open("r", newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            if row.get("http_status") != "200":
                continue
            try:
                payload = int(row["payload_bytes"])
                delta_ns = float(row["delta_ns"])
            except (KeyError, TypeError, ValueError):
                continue
            grouped[payload].append(delta_ns)

    if not grouped:
        raise ValueError(f"No valid HTTP 200 rows found in {csv_path}")

    return dict(grouped)


def value_by_payload(grouped: dict[int, list[float]], stat: str) -> dict[int, float]:
    summary: dict[int, float] = {}
    for payload, values in grouped.items():
        if not values:
            continue

        if stat == "mean":
            value_ns = float(np.mean(values))
        elif stat == "median":
            value_ns = float(statistics.median(values))
        else:
            raise ValueError(f"unsupported stat: {stat}")

        summary[payload] = ns_to_ms(value_ns)

    return summary


def annotate_bars(ax, bars) -> None:
    for bar in bars:
        height = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            height,
            f"{height:.2f}",
            ha="center",
            va="bottom",
            fontsize=8,
            rotation=90,
        )


def main() -> int:
    base_dir = Path(__file__).resolve().parent
    results_dir = base_dir / "results"

    parser = argparse.ArgumentParser(
        description="Plot comparative bar chart for Vanilla vs Prototype gateway keep-alive latencies"
    )
    
    # Try to find default csv files in results directory
    default_vanilla = results_dir / "vanilla_results_custom_32_to_512_step32.csv"
    if not default_vanilla.exists():
        default_vanilla = results_dir / "vanilla_results_step32kb_32_to_1024.csv"
        
    default_proto = results_dir / "proto_results_custom_32_to_512_step32.csv"
    if not default_proto.exists():
        default_proto = results_dir / "proto_results_step32kb_32_to_1024.csv"

    parser.add_argument(
        "--vanilla",
        default=str(default_vanilla),
        help="Path to the Vanilla CSV results file",
    )
    parser.add_argument(
        "--proto",
        default=str(default_proto),
        help="Path to the Prototype CSV results file",
    )
    parser.add_argument(
        "--out",
        default=str(base_dir / "plots" / "vanilla_vs_proto_comparison.png"),
        help="Output path for the generated figure",
    )
    parser.add_argument(
        "--stat",
        choices=["mean", "median"],
        default="mean",
        help="Statistic to plot for each payload size (default: mean)",
    )
    parser.add_argument(
        "--title",
        default="",
        help="Custom chart title",
    )
    args = parser.parse_args()

    vanilla_path = Path(args.vanilla)
    proto_path = Path(args.proto)

    if not vanilla_path.exists():
        print(f"ERROR: Vanilla results file not found at {vanilla_path}", file=sys.stderr)
        return 1
    if not proto_path.exists():
        print(f"ERROR: Prototype results file not found at {proto_path}", file=sys.stderr)
        return 1

    print(f"[plot] Loading vanilla results from: {vanilla_path.name}")
    print(f"[plot] Loading prototype results from: {proto_path.name}")

    vanilla = value_by_payload(load_delta_by_payload(vanilla_path), args.stat)
    proto = value_by_payload(load_delta_by_payload(proto_path), args.stat)

    payloads = sorted(set(vanilla.keys()) & set(proto.keys()))
    if not payloads:
        print("ERROR: no common payload sizes found between the two CSV files.", file=sys.stderr)
        return 1

    labels = [str(payload // 1024) for payload in payloads]
    x = np.arange(len(payloads), dtype=float)
    width = 0.38

    vanilla_vals = np.array([vanilla[payload] for payload in payloads], dtype=float)
    proto_vals = np.array([proto[payload] for payload in payloads], dtype=float)

    fig, ax = plt.subplots(figsize=(14, 7))
    vanilla_bars = ax.bar(
        x - width / 2,
        vanilla_vals,
        width=width,
        color="#1f77b4",
        alpha=0.82,
        label=f"Vanilla {args.stat.capitalize()}",
    )
    proto_bars = ax.bar(
        x + width / 2,
        proto_vals,
        width=width,
        color="#d95f02",
        alpha=0.82,
        label=f"Prototype {args.stat.capitalize()}",
    )

    annotate_bars(ax, vanilla_bars)
    annotate_bars(ax, proto_bars)

    title = args.title or f"Vanilla vs Prototype: {args.stat.capitalize()} time"
    ylabel = f"{args.stat.capitalize()} time (ms)"

    ax.set_title(title, fontsize=15, fontweight="bold", pad=15)
    ax.set_xlabel("Payload Size (KiB)", fontsize=12, labelpad=10)
    ax.set_ylabel(ylabel, fontsize=12, labelpad=10)
    ax.set_xticks(x)  
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.grid(True, axis="y", linestyle="--", alpha=0.3)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, fontsize=11)

    fig.tight_layout()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] Successfully plotted and saved comparison to: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
