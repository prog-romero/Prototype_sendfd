#!/usr/bin/env python3

import argparse
import csv
import math
import os
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.")
    print("Install them with: pip3 install matplotlib numpy")
    sys.exit(1)


METRICS = {
    "body": {"column": "delta_body_ns", "title": "Body completion time"},
    "header": {"column": "delta_header_ns", "title": "Header time"},
    "rtt": {"column": "client_rtt_ns", "title": "End-to-end client time"},
}


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return math.nan
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    rank = (len(ordered) - 1) * pct / 100.0
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return ordered[low]
    weight = rank - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def ns_to_ms(value: float) -> float:
    return value / 1_000_000.0


def size_label(payload: int) -> str:
    if payload >= 1024 * 1024:
        return f"{payload // (1024 * 1024)} MB"
    if payload >= 1024:
        return f"{payload // 1024} KB"
    return f"{payload} B"


def load_results(csv_path: str) -> dict[int, dict[str, list[float]]]:
    if not os.path.exists(csv_path):
        print(f"ERROR: CSV not found: {csv_path}")
        sys.exit(1)

    grouped: dict[int, dict[str, list[float]]] = defaultdict(
        lambda: {
            "delta_header_ns": [],
            "delta_body_ns": [],
            "client_rtt_ns": [],
        }
    )

    with open(csv_path, "r", newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            if row.get("http_status") != "200":
                continue
            try:
                payload = int(row["payload_bytes"])
                grouped[payload]["delta_header_ns"].append(float(row["delta_header_ns"]))
                grouped[payload]["delta_body_ns"].append(float(row["delta_body_ns"]))
                grouped[payload]["client_rtt_ns"].append(float(row["client_rtt_ns"]))
            except (KeyError, TypeError, ValueError):
                continue

    if not grouped:
        print(f"ERROR: no valid HTTP 200 rows found in {csv_path}")
        sys.exit(1)

    return dict(grouped)


def summarize_metric(grouped: dict[int, dict[str, list[float]]], metric_column: str) -> dict[int, dict[str, float]]:
    summary: dict[int, dict[str, float]] = {}
    for payload in sorted(grouped.keys()):
        values = grouped[payload][metric_column]
        if not values:
            continue
        summary[payload] = {
            "mean_ms": ns_to_ms(float(np.mean(values))),
            "std_ms": ns_to_ms(float(np.std(values))),
            "p95_ms": ns_to_ms(percentile(values, 95)),
        }
    return summary


def style_axes(ax):
    ax.grid(True, axis="y", linestyle="--", alpha=0.22)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def plot_mean_time(vanilla_summary: dict[int, dict[str, float]], proto_summary: dict[int, dict[str, float]], metric_name: str, output_file: str):
    payloads = sorted(set(vanilla_summary.keys()) & set(proto_summary.keys()))
    if not payloads:
        print("ERROR: no common payload sizes between vanilla and prototype results")
        sys.exit(1)

    x = np.arange(len(payloads))
    labels = [size_label(payload) for payload in payloads]

    vanilla_mean = np.array([vanilla_summary[p]["mean_ms"] for p in payloads])
    vanilla_std = np.array([vanilla_summary[p]["std_ms"] for p in payloads])
    proto_mean = np.array([proto_summary[p]["mean_ms"] for p in payloads])
    proto_std = np.array([proto_summary[p]["std_ms"] for p in payloads])

    fig, ax = plt.subplots(figsize=(12.5, 6.8))
    vanilla_color = "#1769aa"
    proto_color = "#d9480f"

    ax.plot(x, vanilla_mean, marker="o", markersize=7, linewidth=2.8, color=vanilla_color, label="Vanilla")
    ax.fill_between(x, vanilla_mean - vanilla_std, vanilla_mean + vanilla_std, color=vanilla_color, alpha=0.14)
    ax.plot(x, proto_mean, marker="s", markersize=7, linewidth=2.8, color=proto_color, label="Prototype")
    ax.fill_between(x, proto_mean - proto_std, proto_mean + proto_std, color=proto_color, alpha=0.14)

    for idx in range(len(payloads)):
        gain_pct = 0.0
        if vanilla_mean[idx] > 0:
            gain_pct = (vanilla_mean[idx] - proto_mean[idx]) / vanilla_mean[idx] * 100.0
        ax.text(
            x[idx],
            max(vanilla_mean[idx] + vanilla_std[idx], proto_mean[idx] + proto_std[idx]) * 1.03,
            f"{gain_pct:+.0f}%",
            ha="center",
            va="bottom",
            fontsize=9,
            color="#333333",
        )

    ax.set_title(f"{METRICS[metric_name]['title']} vs request size", fontsize=16, fontweight="bold")
    ax.set_xlabel("Request size", fontsize=12)
    ax.set_ylabel("Time (ms)", fontsize=12)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=28, ha="right")
    style_axes(ax)
    ax.legend(frameon=False, loc="upper left")

    fig.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"[✓] Figure saved: {output_file}")


def plot_histogram(vanilla_grouped: dict[int, dict[str, list[float]]], proto_grouped: dict[int, dict[str, list[float]]], metric_name: str, payload: int, output_file: str):
    metric_column = METRICS[metric_name]["column"]
    if payload not in vanilla_grouped or payload not in proto_grouped:
        print(f"ERROR: payload {payload} not found in both datasets")
        sys.exit(1)

    vanilla_values = np.array([ns_to_ms(v) for v in vanilla_grouped[payload][metric_column]], dtype=float)
    proto_values = np.array([ns_to_ms(v) for v in proto_grouped[payload][metric_column]], dtype=float)

    lo = min(vanilla_values.min(), proto_values.min())
    hi = max(vanilla_values.max(), proto_values.max())
    bins = np.linspace(lo, hi, 16)

    fig, ax = plt.subplots(figsize=(10.5, 6.2))
    vanilla_color = "#1769aa"
    proto_color = "#d9480f"

    ax.hist(vanilla_values, bins=bins, color=vanilla_color, alpha=0.55, edgecolor="white", linewidth=0.8, label="Vanilla")
    ax.hist(proto_values, bins=bins, color=proto_color, alpha=0.55, edgecolor="white", linewidth=0.8, label="Prototype")
    ax.axvline(np.mean(vanilla_values), color=vanilla_color, linestyle="--", linewidth=2.0)
    ax.axvline(np.mean(proto_values), color=proto_color, linestyle="--", linewidth=2.0)

    ax.set_title(f"{METRICS[metric_name]['title']} distribution at {size_label(payload)}", fontsize=15, fontweight="bold")
    ax.set_xlabel("Time (ms)", fontsize=12)
    ax.set_ylabel("Requests", fontsize=12)
    style_axes(ax)
    ax.legend(frameon=False, loc="upper right")

    note = f"mean vanilla = {np.mean(vanilla_values):.3f} ms\nmean prototype = {np.mean(proto_values):.3f} ms"
    ax.text(
        0.98,
        0.95,
        note,
        transform=ax.transAxes,
        ha="right",
        va="top",
        fontsize=10,
        bbox={"boxstyle": "round,pad=0.35", "facecolor": "white", "alpha": 0.88, "edgecolor": "#cccccc"},
    )

    fig.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"[✓] Figure saved: {output_file}")


def print_summary(vanilla_summary: dict[int, dict[str, float]], proto_summary: dict[int, dict[str, float]], metric_name: str):
    payloads = sorted(set(vanilla_summary.keys()) & set(proto_summary.keys()))
    print(f"\nAggregated summary for {METRICS[metric_name]['title'].lower()}\n")
    print(
        "payload".ljust(10)
        + "vanilla_mean".rjust(16)
        + "proto_mean".rjust(16)
        + "gain".rjust(12)
        + "vanilla_p95".rjust(16)
        + "proto_p95".rjust(16)
    )
    print("-" * 86)
    for payload in payloads:
        vanilla_mean = vanilla_summary[payload]["mean_ms"]
        proto_mean = proto_summary[payload]["mean_ms"]
        vanilla_p95 = vanilla_summary[payload]["p95_ms"]
        proto_p95 = proto_summary[payload]["p95_ms"]
        gain = ((vanilla_mean - proto_mean) / vanilla_mean * 100.0) if vanilla_mean > 0 else 0.0
        print(
            size_label(payload).ljust(10)
            + f"{vanilla_mean:16.3f}"
            + f"{proto_mean:16.3f}"
            + f"{gain:11.1f}%"
            + f"{vanilla_p95:16.3f}"
            + f"{proto_p95:16.3f}"
        )


def main() -> int:
    base_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Generate two comparison figures for vanilla and prototype benchmark results")
    parser.add_argument("--vanilla", default=str(base_dir / "vanilla_results.csv"), help="Path to vanilla CSV file")
    parser.add_argument("--proto", default=str(base_dir / "proto_results.csv"), help="Path to prototype CSV file")
    parser.add_argument("--metric", choices=sorted(METRICS.keys()), default="body", help="Metric to visualize")
    parser.add_argument("--hist-payload", type=int, default=1048576, help="Payload size in bytes for the histogram figure")
    parser.add_argument("--curve-output", default=str(base_dir / "time_mean_vs_size.png"), help="Output path for the mean-time figure")
    parser.add_argument("--hist-output", default=str(base_dir / "time_histogram.png"), help="Output path for the histogram figure")
    parser.add_argument("--no-display", action="store_true", help="Do not open the figures interactively")
    args = parser.parse_args()

    vanilla_grouped = load_results(args.vanilla)
    proto_grouped = load_results(args.proto)
    metric_column = METRICS[args.metric]["column"]
    vanilla_summary = summarize_metric(vanilla_grouped, metric_column)
    proto_summary = summarize_metric(proto_grouped, metric_column)

    print_summary(vanilla_summary, proto_summary, args.metric)
    plot_mean_time(vanilla_summary, proto_summary, args.metric, args.curve_output)
    plot_histogram(vanilla_grouped, proto_grouped, args.metric, args.hist_payload, args.hist_output)

    if not args.no_display:
        plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())