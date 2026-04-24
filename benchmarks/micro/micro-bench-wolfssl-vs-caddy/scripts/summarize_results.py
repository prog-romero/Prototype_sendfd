#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import os
from collections import defaultdict


def percentile(sorted_values: list[int], pct: float) -> int:
    if not sorted_values:
        return 0
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (len(sorted_values) - 1) * pct
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return sorted_values[lower]
    weight = rank - lower
    return int(sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight)


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize the raw wolfSSL vs Caddy benchmark CSV")
    parser.add_argument("--in", dest="input_path", required=True)
    parser.add_argument("--out", dest="output_path", required=True)
    args = parser.parse_args()

    groups: dict[tuple[str, int], list[int]] = defaultdict(list)

    with open(args.input_path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            groups[(row["implementation"], int(row["payload_size"]))].append(int(row["delta_ns"]))

    rows: list[dict[str, int | str | float]] = []
    for (implementation, payload_size), values in sorted(groups.items()):
        values.sort()
        rows.append(
            {
                "implementation": implementation,
                "payload_size": payload_size,
                "samples": len(values),
                "min_ns": values[0],
                "mean_ns": int(sum(values) / len(values)),
                "p50_ns": percentile(values, 0.50),
                "p95_ns": percentile(values, 0.95),
                "p99_ns": percentile(values, 0.99),
                "max_ns": values[-1],
            }
        )

    os.makedirs(os.path.dirname(os.path.abspath(args.output_path)), exist_ok=True)
    with open(args.output_path, "w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "implementation",
                "payload_size",
                "samples",
                "min_ns",
                "mean_ns",
                "p50_ns",
                "p95_ns",
                "p99_ns",
                "max_ns",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {len(rows)} summary rows to {args.output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())