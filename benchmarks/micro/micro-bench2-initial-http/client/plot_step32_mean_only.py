#!/usr/bin/env python3

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
    print("ERROR: matplotlib and numpy are required.", file=sys.stderr)
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
        raise ValueError(f"no valid HTTP 200 rows found in {csv_path}")

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
            f"{height:.1f}",
            ha="center",
            va="bottom",
            fontsize=8,
            rotation=90,
        )


def main() -> int:
    base_dir = Path(__file__).resolve().parent

    parser = argparse.ArgumentParser(
        description="Simple step32 comparison plot: mean bars only for vanilla vs prototype"
    )
    parser.add_argument(
        "--vanilla",
        default=str(base_dir / "vanilla_results_step32kb_32_to_1024.csv"),
        help="Vanilla step32 CSV path",
    )
    parser.add_argument(
        "--proto",
        default=str(base_dir / "proto_results_step32kb_32_to_1024.csv"),
        help="Prototype step32 CSV path",
    )
    parser.add_argument(
        "--out",
        default=str(base_dir / "vanilla_vs_proto_step32_mean_only.png"),
        help="Output figure path",
    )
    parser.add_argument(
        "--stat",
        choices=["mean", "median"],
        default="mean",
        help="Statistic to plot for each payload size",
    )
    parser.add_argument(
        "--title",
        default="",
        help="Figure title",
    )
    args = parser.parse_args()

    vanilla = value_by_payload(load_delta_by_payload(Path(args.vanilla)), args.stat)
    proto = value_by_payload(load_delta_by_payload(Path(args.proto)), args.stat)

    payloads = sorted(set(vanilla.keys()) & set(proto.keys()))
    if not payloads:
        print("ERROR: no common payload sizes between vanilla and prototype", file=sys.stderr)
        return 1

    labels = [str(payload // 1024) for payload in payloads]
    x = np.arange(len(payloads), dtype=float)
    width = 0.38

    vanilla_mean = np.array([vanilla[payload] for payload in payloads], dtype=float)
    proto_mean = np.array([proto[payload] for payload in payloads], dtype=float)
    stat_label = args.stat.capitalize()

    fig, ax = plt.subplots(figsize=(14, 7))
    vanilla_bars = ax.bar(
        x - width / 2,
        vanilla_mean,
        width=width,
        color="#1f77b4",
        alpha=0.82,
        label=f"Vanilla {args.stat}",
    )
    proto_bars = ax.bar(
        x + width / 2,
        proto_mean,
        width=width,
        color="#d95f02",
        alpha=0.82,
        label=f"Prototype {args.stat}",
    )

    annotate_bars(ax, vanilla_bars)
    annotate_bars(ax, proto_bars)

    title = args.title or f"Step 32 KiB: {args.stat} Top1 to Top2 time"
    ylabel = f"{args.stat.capitalize()} Top1 to Top2 time (ms)"

    ax.set_title(title, fontsize=15, fontweight="bold")
    ax.set_xlabel("Payload size (KiB)")
    ax.set_ylabel(ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.grid(True, axis="y", linestyle="--", alpha=0.25)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False)

    fig.tight_layout()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] figure saved: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())