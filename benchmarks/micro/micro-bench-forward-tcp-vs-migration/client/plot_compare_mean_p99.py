#!/usr/bin/env python3
"""Plot a simple vanilla vs prototype comparison from two benchmark CSV files.

The figure uses:
- grouped bars for mean time
- overlayed curves for median time
- mean value labels above each bar

By default it reads the uniform runs generated in ../results/.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    print("matplotlib is required: pip install matplotlib", file=sys.stderr)
    raise SystemExit(1)


REQUIRED_COLUMNS = {"payload_size", "delta_ns"}
PAYLOAD_ORDER = [64, 256, 1024, 4096, 16384, 65536, 131072, 262144, 524288, 1048576]

BAR_COLORS = {
    "vanilla": "#5B6C8F",
    "proto": "#D8734A",
}

LINE_COLORS = {
    "vanilla": "#2F4468",
    "proto": "#A54722",
}


def default_results_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "results"


def default_input(name_uniform: str, name_mixed: str) -> Path:
    results_dir = default_results_dir()
    uniform = results_dir / name_uniform
    if uniform.exists():
        return uniform
    return results_dir / name_mixed


def human_bytes(value: int) -> str:
    if value >= 1024 * 1024 and value % (1024 * 1024) == 0:
        return f"{value // (1024 * 1024)} MiB"
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024} KiB"
    return f"{value} B"


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        raise ValueError("cannot compute percentile of empty list")

    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]

    rank = (len(ordered) - 1) * quantile
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]

    fraction = rank - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def load_samples(path: Path) -> dict[int, list[float]]:
    if not path.exists():
        raise SystemExit(f"missing input CSV: {path}")

    grouped: dict[int, list[float]] = defaultdict(list)
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = REQUIRED_COLUMNS - set(reader.fieldnames or [])
        if missing:
            raise SystemExit(f"{path} is missing required columns: {', '.join(sorted(missing))}")

        for row in reader:
            try:
                payload_size = int(row["payload_size"])
                delta_ms = float(row["delta_ns"]) / 1_000_000.0
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"bad row in {path}: {exc}") from exc
            grouped[payload_size].append(delta_ms)

    if not grouped:
        raise SystemExit(f"no samples found in {path}")

    return dict(grouped)


def build_stats(samples: dict[int, list[float]]) -> dict[int, tuple[float, float]]:
    stats: dict[int, tuple[float, float]] = {}
    for payload_size, values in samples.items():
        mean_ms = sum(values) / len(values)
        median_ms = percentile(values, 0.50)
        stats[payload_size] = (mean_ms, median_ms)
    return stats


def ordered_payloads(*datasets: dict[int, tuple[float, float]]) -> list[int]:
    payloads = set()
    for dataset in datasets:
        payloads.update(dataset.keys())

    ordered = [size for size in PAYLOAD_ORDER if size in payloads]
    remaining = sorted(size for size in payloads if size not in PAYLOAD_ORDER)
    return ordered + remaining


def format_ms(value: float) -> str:
    if value >= 100:
        return f"{value:.1f}"
    if value >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def add_bar_labels(ax: plt.Axes, bars, use_log_y: bool) -> None:
    heights = [bar.get_height() for bar in bars if bar.get_height() > 0]
    if not heights:
        return

    linear_offset = max(heights) * 0.015
    if linear_offset == 0:
        linear_offset = 0.05

    for bar in bars:
        height = bar.get_height()
        if height <= 0:
            continue

        x_pos = bar.get_x() + bar.get_width() / 2
        y_pos = height * 1.04 if use_log_y else height + linear_offset
        ax.text(
            x_pos,
            y_pos,
            format_ms(height),
            ha="center",
            va="bottom",
            fontsize=8,
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot vanilla vs prototype mean bars and median curves")
    parser.add_argument(
        "--vanilla",
        type=Path,
        default=default_input("combined_vanilla_uniform.csv", "combined_vanilla.csv"),
        help="path to the vanilla CSV",
    )
    parser.add_argument(
        "--proto",
        type=Path,
        default=default_input("combined_proto_uniform.csv", "combined_proto.csv"),
        help="path to the prototype CSV",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=default_results_dir() / "compare_mean_median.png",
        help="output image path",
    )
    parser.add_argument(
        "--title",
        default="",
        help="optional figure title",
    )
    parser.add_argument(
        "--log-y",
        action="store_true",
        help="use a logarithmic Y axis",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    vanilla_stats = build_stats(load_samples(args.vanilla.expanduser().resolve()))
    proto_stats = build_stats(load_samples(args.proto.expanduser().resolve()))
    payloads = ordered_payloads(vanilla_stats, proto_stats)

    x = list(range(len(payloads)))
    width = 0.34

    vanilla_mean = [vanilla_stats[size][0] for size in payloads]
    vanilla_median = [vanilla_stats[size][1] for size in payloads]
    proto_mean = [proto_stats[size][0] for size in payloads]
    proto_median = [proto_stats[size][1] for size in payloads]

    fig, ax = plt.subplots(figsize=(10, 5.8))

    vanilla_x = [value - width / 2 for value in x]
    proto_x = [value + width / 2 for value in x]

    vanilla_bars = ax.bar(
        vanilla_x,
        vanilla_mean,
        width=width,
        color=BAR_COLORS["vanilla"],
        alpha=0.55,
        edgecolor=LINE_COLORS["vanilla"],
        linewidth=1.0,
        label="Vanilla mean",
    )
    proto_bars = ax.bar(
        proto_x,
        proto_mean,
        width=width,
        color=BAR_COLORS["proto"],
        alpha=0.55,
        edgecolor=LINE_COLORS["proto"],
        linewidth=1.0,
        label="Proto mean",
    )

    ax.plot(
        vanilla_x,
        vanilla_median,
        color=LINE_COLORS["vanilla"],
        marker="o",
        linewidth=2.0,
        markersize=5,
        label="Vanilla median",
    )
    ax.plot(
        proto_x,
        proto_median,
        color=LINE_COLORS["proto"],
        marker="o",
        linewidth=2.0,
        markersize=5,
        label="Proto median",
    )

    if args.title:
        ax.set_title(args.title)

    ax.set_xlabel("Payload size")
    ax.set_ylabel("Time (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels([human_bytes(size) for size in payloads], rotation=30, ha="right")

    if args.log_y:
        ax.set_yscale("log")

    add_bar_labels(ax, vanilla_bars, args.log_y)
    add_bar_labels(ax, proto_bars, args.log_y)

    ax.grid(axis="y", alpha=0.3)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, ncol=2)

    args.out.expanduser().resolve().parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(args.out.expanduser().resolve(), dpi=220, bbox_inches="tight")
    plt.close(fig)

    print(args.out.expanduser().resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())