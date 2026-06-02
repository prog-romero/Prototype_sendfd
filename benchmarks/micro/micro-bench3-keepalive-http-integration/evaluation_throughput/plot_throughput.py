#!/usr/bin/env python3
"""
plot_throughput.py — Compare Vanilla vs Prototype throughput and latency across concurrency levels.

Usage:
    python3 plot_throughput.py --vanilla results/vanilla_2core_32kb.csv --proto results/proto_2core_32kb.csv --out plots/throughput_comparison_2core.png
"""

import argparse
import csv
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required. Install with: pip3 install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


def load_throughput_csv(csv_path: Path) -> dict:
    data = {}
    with csv_path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                c = int(row["concurrency"])
                data[c] = {
                    "rps": float(row["rps"]),
                    "transfer_mb_s": float(row["transfer_mb_s"]),
                    "lat_avg_ms": float(row["lat_avg_ms"]),
                    "p50_ms": float(row["p50_ms"]),
                    "p99_ms": float(row["p99_ms"]),
                    "errors": int(row["errors_non2xx"]),
                }
            except (KeyError, ValueError, TypeError):
                continue
    if not data:
        raise ValueError(f"No valid data rows found in {csv_path}")
    return data


def main():
    base_dir = Path(__file__).resolve().parent
    results_dir = base_dir / "results"
    plots_dir = base_dir / "plots"

    parser = argparse.ArgumentParser(
        description="Plot max-throughput benchmark comparisons (Vanilla vs Prototype)"
    )
    parser.add_argument(
        "--vanilla",
        default=str(results_dir / "vanilla_2core_32kb.csv"),
        help="Path to Vanilla throughput CSV",
    )
    parser.add_argument(
        "--proto",
        default=str(results_dir / "proto_2core_32kb.csv"),
        help="Path to Prototype throughput CSV",
    )
    parser.add_argument(
        "--out",
        default=str(plots_dir / "throughput_vs_latency_comparison.png"),
        help="Path to save the generated figure",
    )
    parser.add_argument(
        "--title",
        default="",
        help="Custom figure title",
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

    print(f"[plot] Loading vanilla results: {vanilla_path.name}")
    print(f"[plot] Loading prototype results: {proto_path.name}")

    vanilla_data = load_throughput_csv(vanilla_path)
    proto_data = load_throughput_csv(proto_path)

    # Find common concurrency levels
    concurrencies = sorted(set(vanilla_data.keys()) & set(proto_data.keys()))
    if not concurrencies:
        print("ERROR: No common concurrency levels found between Vanilla and Prototype CSV files.", file=sys.stderr)
        return 1

    vanilla_rps = [vanilla_data[c]["rps"] for c in concurrencies]
    proto_rps = [proto_data[c]["rps"] for c in concurrencies]

    vanilla_p99 = [vanilla_data[c]["p99_ms"] for c in concurrencies]
    proto_p99 = [proto_data[c]["p99_ms"] for c in concurrencies]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))

    # --- Plot 1: Throughput (RPS) ---
    ax1.plot(concurrencies, vanilla_rps, label="Vanilla Gateway", color="#1f77b4", marker="o", linewidth=2, markersize=6)
    ax1.plot(concurrencies, proto_rps, label="Prototype SCM_RIGHTS", color="#d95f02", marker="s", linewidth=2, markersize=6)
    ax1.set_xlabel("Concurrency (Concurrent Connections)", fontsize=11, labelpad=8)
    ax1.set_ylabel("Throughput (Requests/sec)", fontsize=11, labelpad=8)
    ax1.set_title("Throughput vs Concurrency", fontsize=13, fontweight="bold", pad=10)
    ax1.grid(True, linestyle="--", alpha=0.3)
    ax1.legend(frameon=False, fontsize=10)

    # --- Plot 2: P99 Latency (ms) ---
    ax2.plot(concurrencies, vanilla_p99, label="Vanilla Gateway", color="#1f77b4", marker="o", linewidth=2, markersize=6)
    ax2.plot(concurrencies, proto_p99, label="Prototype SCM_RIGHTS", color="#d95f02", marker="s", linewidth=2, markersize=6)
    ax2.set_xlabel("Concurrency (Concurrent Connections)", fontsize=11, labelpad=8)
    ax2.set_ylabel("P99 Latency (ms)", fontsize=11, labelpad=8)
    ax2.set_title("P99 Latency vs Concurrency", fontsize=13, fontweight="bold", pad=10)
    ax2.grid(True, linestyle="--", alpha=0.3)
    ax2.legend(frameon=False, fontsize=10)

    title = args.title or "Vanilla vs SCM_RIGHTS Prototype: Throughput & Latency Scaling"
    fig.suptitle(title, fontsize=16, fontweight="bold", y=0.98)
    fig.tight_layout()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)

    print(f"[ok] Successfully plotted and saved comparison to: {out_path}")
    return 0


if __name__ == "__main__":
    main()
