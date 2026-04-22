#!/usr/bin/env python3
"""Plot vanilla vs prototype mean bars with decryption stacked onto vanilla.

The figure shows, for each payload size:
- vanilla forward mean as the base bar
- decryption mean stacked on top of vanilla in green
- prototype mean as a separate bar

No median or p99 curves are drawn in this figure.
"""

from __future__ import annotations

import argparse
import csv
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


FORWARD_REQUIRED_COLUMNS = {"payload_size", "delta_ns"}
PAYLOAD_ORDER = [64, 256, 1024, 4096, 16384, 65536, 131072, 262144, 524288, 1048576]

COLORS = {
    "vanilla_forward": "#5B6C8F",
    "decrypt": "#5B8E7D",
    "proto": "#D8734A",
}

EDGES = {
    "vanilla_forward": "#2F4468",
    "decrypt": "#173B3F",
    "proto": "#A54722",
}


def forward_results_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "results"


def decrypt_results_dir() -> Path:
    return Path(__file__).resolve().parent.parent.parent / "micro-bench-tls-decryption-cost" / "results"


def latest_match(results_dir: Path, pattern: str) -> Path | None:
    matches = sorted(results_dir.glob(pattern), key=lambda path: path.stat().st_mtime)
    return matches[-1] if matches else None


def default_forward_input(name_uniform: str, name_mixed: str) -> Path:
    results_dir = forward_results_dir()
    uniform = results_dir / name_uniform
    if uniform.exists():
        return uniform
    return results_dir / name_mixed


def default_decrypt_input() -> Path | None:
    results_dir = decrypt_results_dir()
    latest_summary = latest_match(results_dir, "decrypt_summary_*.csv")
    if latest_summary is not None:
        return latest_summary
    latest_raw = latest_match(results_dir, "decrypt_raw_*.csv")
    if latest_raw is not None:
        return latest_raw
    return None


def human_bytes(value: int) -> str:
    if value >= 1024 * 1024 and value % (1024 * 1024) == 0:
        return f"{value // (1024 * 1024)} MiB"
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024} KiB"
    return f"{value} B"


def format_ms(value: float) -> str:
    if value >= 100:
        return f"{value:.1f}"
    if value >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def load_forward_samples(path: Path) -> dict[int, list[float]]:
    if not path.exists():
        raise SystemExit(f"missing input CSV: {path}")

    grouped: dict[int, list[float]] = defaultdict(list)
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = FORWARD_REQUIRED_COLUMNS - set(reader.fieldnames or [])
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


def build_forward_means(samples: dict[int, list[float]]) -> dict[int, float]:
    means: dict[int, float] = {}
    for payload_size, values in samples.items():
        means[payload_size] = sum(values) / len(values)
    return means


def load_decrypt_means(path: Path) -> dict[int, float]:
    if not path.exists():
        raise SystemExit(f"missing decryption CSV: {path}")

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = set(reader.fieldnames or [])

        if "payload_size" not in fieldnames:
            raise SystemExit(f"{path} is missing required column: payload_size")

        rows = list(reader)
        if not rows:
            raise SystemExit(f"no rows found in {path}")

    if "mean_us" in fieldnames:
        return {
            int(row["payload_size"]): float(row["mean_us"]) / 1000.0
            for row in rows
        }

    if "mean_ns" in fieldnames:
        return {
            int(row["payload_size"]): float(row["mean_ns"]) / 1_000_000.0
            for row in rows
        }

    grouped: dict[int, list[float]] = defaultdict(list)
    if "delta_us" in fieldnames:
        for row in rows:
            grouped[int(row["payload_size"])].append(float(row["delta_us"]) / 1000.0)
    elif "delta_ns" in fieldnames:
        for row in rows:
            grouped[int(row["payload_size"])].append(float(row["delta_ns"]) / 1_000_000.0)
    else:
        raise SystemExit(
            f"{path} must contain one of: mean_us, mean_ns, delta_us, delta_ns"
        )

    return {
        payload_size: sum(values) / len(values)
        for payload_size, values in grouped.items()
    }


def ordered_payloads(*datasets: dict[int, float]) -> list[int]:
    payloads = set()
    for dataset in datasets:
        payloads.update(dataset.keys())

    ordered = [size for size in PAYLOAD_ORDER if size in payloads]
    remaining = sorted(size for size in payloads if size not in PAYLOAD_ORDER)
    return ordered + remaining


def add_top_labels(ax: plt.Axes, x_positions: list[float], heights: list[float]) -> None:
    if not heights:
        return

    positive_heights = [height for height in heights if height > 0]
    if not positive_heights:
        return

    offset = max(positive_heights) * 0.015
    if offset == 0:
        offset = 0.05

    for x_pos, height in zip(x_positions, heights):
        if height <= 0:
            continue
        ax.text(
            x_pos,
            height + offset,
            format_ms(height),
            ha="center",
            va="bottom",
            fontsize=8,
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot vanilla mean with stacked decryption cost against prototype mean"
    )
    parser.add_argument(
        "--vanilla",
        type=Path,
        default=default_forward_input("combined_vanilla_uniform.csv", "combined_vanilla.csv"),
        help="path to the vanilla CSV",
    )
    parser.add_argument(
        "--proto",
        type=Path,
        default=default_forward_input("combined_proto_uniform.csv", "combined_proto.csv"),
        help="path to the prototype CSV",
    )
    parser.add_argument(
        "--decrypt",
        type=Path,
        default=default_decrypt_input(),
        help="path to the decryption benchmark CSV (summary or raw)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=forward_results_dir() / "compare_mean_with_decrypt.png",
        help="output image path",
    )
    parser.add_argument(
        "--title",
        default="",
        help="optional figure title",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.decrypt is None:
        raise SystemExit(
            "no decryption CSV found automatically; pass it explicitly with --decrypt"
        )

    vanilla_means = build_forward_means(load_forward_samples(args.vanilla.expanduser().resolve()))
    proto_means = build_forward_means(load_forward_samples(args.proto.expanduser().resolve()))
    decrypt_means = load_decrypt_means(args.decrypt.expanduser().resolve())

    payloads = ordered_payloads(vanilla_means, proto_means)
    missing_decrypt = [size for size in payloads if size not in decrypt_means]
    if missing_decrypt:
        labels = ", ".join(human_bytes(size) for size in missing_decrypt)
        raise SystemExit(f"decryption CSV is missing payload sizes: {labels}")

    x = list(range(len(payloads)))
    width = 0.34

    vanilla_base = [vanilla_means[size] for size in payloads]
    decrypt_stack = [decrypt_means[size] for size in payloads]
    vanilla_total = [base + extra for base, extra in zip(vanilla_base, decrypt_stack)]
    proto_values = [proto_means[size] for size in payloads]

    fig, ax = plt.subplots(figsize=(10, 5.8))

    vanilla_x = [value - width / 2 for value in x]
    proto_x = [value + width / 2 for value in x]

    ax.bar(
        vanilla_x,
        vanilla_base,
        width=width,
        color=COLORS["vanilla_forward"],
        alpha=0.55,
        edgecolor=EDGES["vanilla_forward"],
        linewidth=1.0,
        label="Vanilla forward",
    )
    ax.bar(
        vanilla_x,
        decrypt_stack,
        width=width,
        bottom=vanilla_base,
        color=COLORS["decrypt"],
        alpha=0.70,
        edgecolor=EDGES["decrypt"],
        linewidth=1.0,
        label="Decrypt",
    )
    ax.bar(
        proto_x,
        proto_values,
        width=width,
        color=COLORS["proto"],
        alpha=0.55,
        edgecolor=EDGES["proto"],
        linewidth=1.0,
        label="Proto",
    )

    add_top_labels(ax, vanilla_x, vanilla_total)
    add_top_labels(ax, proto_x, proto_values)

    if args.title:
        ax.set_title(args.title)

    ax.set_xlabel("Payload size")
    ax.set_ylabel("Time (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels([human_bytes(size) for size in payloads], rotation=30, ha="right")
    ax.grid(axis="y", alpha=0.3)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, ncol=3)

    output_path = args.out.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(output_path, dpi=220, bbox_inches="tight")
    plt.close(fig)

    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
