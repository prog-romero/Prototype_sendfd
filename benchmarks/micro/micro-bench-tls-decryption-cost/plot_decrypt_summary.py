#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path
from typing import Dict, List, Optional

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ModuleNotFoundError as exc:
    raise SystemExit(
        "matplotlib is required. Install it with: python3 -m pip install matplotlib"
    ) from exc


VALID_METRICS = {"mean_us", "p50_us", "p95_us", "p99_us", "max_us"}

METRIC_LABELS = {
    "mean_us": "Mean latency (us)",
    "p50_us": "P50 latency (us)",
    "p95_us": "P95 latency (us)",
    "p99_us": "P99 latency (us)",
    "max_us": "Max latency (us)",
}


def human_size(num_bytes: int) -> str:
    if num_bytes >= 1024 * 1024 and num_bytes % (1024 * 1024) == 0:
        return f"{num_bytes // (1024 * 1024)} MiB"
    if num_bytes >= 1024 and num_bytes % 1024 == 0:
        return f"{num_bytes // 1024} KiB"
    return f"{num_bytes} B"


def compact_latency_us(value_us: float) -> str:
    if value_us >= 1000.0:
        return f"{value_us / 1000.0:.1f} ms"
    return f"{value_us:.1f} us"


def load_summary_rows(csv_path: Path) -> List[Dict[str, object]]:
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = set(reader.fieldnames or [])
        required = {"payload_size", "tls_version", "cipher"} | VALID_METRICS
        missing = required - fieldnames
        if missing:
            raise SystemExit(
                f"Missing expected CSV columns in {csv_path}: {', '.join(sorted(missing))}"
            )

        rows: List[Dict[str, object]] = []
        for row in reader:
            rows.append(
                {
                    "payload_size": int(row["payload_size"]),
                    "tls_version": row["tls_version"],
                    "cipher": row["cipher"],
                    "mean_us": float(row["mean_us"]),
                    "p50_us": float(row["p50_us"]),
                    "p95_us": float(row["p95_us"]),
                    "p99_us": float(row["p99_us"]),
                    "max_us": float(row["max_us"]),
                }
            )

    rows.sort(key=lambda item: int(item["payload_size"]))
    return rows


def default_output_path(csv_path: Path, metric: str) -> Path:
    return csv_path.with_name(f"{csv_path.stem}_{metric}.png")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Render a bar chart from a TLS decrypt benchmark summary CSV."
    )
    parser.add_argument("summary_csv", type=Path, help="Path to decrypt_summary_*.csv")
    parser.add_argument(
        "--metric",
        choices=sorted(VALID_METRICS),
        default="mean_us",
        help="Summary column to plot",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output PNG path. Default: <summary_csv stem>_<metric>.png",
    )
    parser.add_argument(
        "--linear",
        action="store_true",
        help="Use a linear Y axis instead of logarithmic",
    )
    parser.add_argument(
        "--title",
        default=None,
        help="Optional custom chart title",
    )
    return parser


def render_plot(
    rows: List[Dict[str, object]],
    metric: str,
    output_path: Path,
    use_linear_scale: bool,
    title: Optional[str],
) -> None:
    labels = [human_size(int(row["payload_size"])) for row in rows]
    values = [float(row[metric]) for row in rows]
    tls_version = str(rows[0]["tls_version"]) if rows else ""
    cipher = str(rows[0]["cipher"]) if rows else ""

    figure, axis = plt.subplots(figsize=(11, 6.5))
    bars = axis.bar(
        labels,
        values,
        color="#5B8E7D",
        edgecolor="#173B3F",
        linewidth=1.0,
        width=0.72,
    )

    axis.set_xlabel("Payload size")
    axis.set_ylabel(METRIC_LABELS[metric])
    axis.set_title(title or f"TLS Decryption Cost - {metric}", fontsize=16, pad=20)
    axis.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.35)
    axis.set_axisbelow(True)

    if not use_linear_scale:
        axis.set_yscale("log")

    subtitle = f"TLS {tls_version} | {cipher}" if tls_version or cipher else ""
    if subtitle:
        figure.text(0.5, 0.93, subtitle, ha="center", va="center", fontsize=10)

    for bar, value in zip(bars, values):
        x_pos = bar.get_x() + bar.get_width() / 2.0
        if use_linear_scale:
            y_pos = value + max(values) * 0.015
        else:
            y_pos = value * 1.12
        axis.text(
            x_pos,
            y_pos,
            compact_latency_us(value),
            ha="center",
            va="bottom",
            rotation=0,
            fontsize=9,
            color="#0F172A",
        )

    plt.xticks(rotation=20)
    figure.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output_path, dpi=220, bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if not args.summary_csv.is_file():
        raise SystemExit(f"Summary CSV not found: {args.summary_csv}")

    rows = load_summary_rows(args.summary_csv)
    if not rows:
        raise SystemExit(f"Summary CSV is empty: {args.summary_csv}")

    output_path = args.output or default_output_path(args.summary_csv, args.metric)
    render_plot(rows, args.metric, output_path, args.linear, args.title)
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())