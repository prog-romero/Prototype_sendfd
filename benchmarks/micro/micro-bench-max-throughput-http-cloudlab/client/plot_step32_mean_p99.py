#!/usr/bin/env python3

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.", file=sys.stderr)
    sys.exit(1)


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


def summarize(grouped: dict[int, list[float]]) -> dict[int, tuple[float, float]]:
    summary: dict[int, tuple[float, float]] = {}
    for payload, values in grouped.items():
        summary[payload] = (
            ns_to_ms(float(np.mean(values))),
            ns_to_ms(percentile(values, 99)),
        )
    return summary


def main() -> int:
    base_dir = Path(__file__).resolve().parent

    parser = argparse.ArgumentParser(
        description="Simple step32 comparison plot: mean bars and p99 curves for vanilla vs prototype"
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
        default=str(base_dir / "vanilla_vs_proto_step32_mean_p99.png"),
        help="Output figure path",
    )
    parser.add_argument(
        "--title",
        default="Step 32 KiB: mean bars and p99 curves",
        help="Figure title",
    )
    args = parser.parse_args()

    vanilla = summarize(load_delta_by_payload(Path(args.vanilla)))
    proto = summarize(load_delta_by_payload(Path(args.proto)))

    payloads = sorted(set(vanilla.keys()) & set(proto.keys()))
    if not payloads:
        print("ERROR: no common payload sizes between vanilla and prototype", file=sys.stderr)
        return 1

    labels = [str(payload // 1024) for payload in payloads]
    x = np.arange(len(payloads), dtype=float)
    width = 0.36

    vanilla_mean = np.array([vanilla[payload][0] for payload in payloads], dtype=float)
    proto_mean = np.array([proto[payload][0] for payload in payloads], dtype=float)
    vanilla_p99 = np.array([vanilla[payload][1] for payload in payloads], dtype=float)
    proto_p99 = np.array([proto[payload][1] for payload in payloads], dtype=float)

    vanilla_color = "#1f77b4"
    proto_color = "#d95f02"

    fig, ax = plt.subplots(figsize=(14, 7))
    ax.bar(x - width / 2, vanilla_mean, width=width, color=vanilla_color, alpha=0.78, label="Vanilla mean")
    ax.bar(x + width / 2, proto_mean, width=width, color=proto_color, alpha=0.78, label="Prototype mean")

    ax.plot(x, vanilla_p99, color=vanilla_color, marker="o", linewidth=2.1, label="Vanilla p99")
    ax.plot(x, proto_p99, color=proto_color, marker="s", linewidth=2.1, label="Prototype p99")

    ax.set_title(args.title, fontsize=15, fontweight="bold")
    ax.set_xlabel("Payload size (KiB)")
    ax.set_ylabel("Top1 to Top2 time (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.grid(True, axis="y", linestyle="--", alpha=0.25)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, ncol=2)

    fig.tight_layout()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] figure saved: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())