#!/usr/bin/env python3
"""
plot_pidstat.py — Plot per-component CPU and RAM usage across a rate sweep.

Reads pidstat/<mode>/{cpu,ram}/<component>.csv (one row per rate step, as
produced by sweep_throughput_wrk2.py) and plots each metric as one figure,
with one line per component.

Usage:
    python3 plot_pidstat.py --mode proto
    python3 plot_pidstat.py --mode vanilla --out-dir plots/pidstat_vanilla
"""

import argparse
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import pandas as pd
except ImportError:
    print("ERROR: matplotlib and pandas are required. Install with: pip3 install matplotlib pandas", file=sys.stderr)
    sys.exit(1)

# sweep_throughput_wrk2.py / collect_pidstat.sh use "prototype" as the on-disk
# folder name for --mode proto.
MODE_DIR = {"vanilla": "vanilla", "proto": "prototype"}

CPU_METRICS = [
    ("usr_pct", "user CPU (%)"),
    ("system_pct", "system CPU (%)"),
    ("cpu_pct", "total CPU (%)"),
]
RAM_METRICS = [
    ("rss_kb", "RSS memory (KB)"),
    ("mem_pct", "memory (%)"),
]


def load_components(csv_dir: Path) -> dict[str, "pd.DataFrame"]:
    components = {}
    for csv_path in sorted(csv_dir.glob("*.csv")):
        components[csv_path.stem] = pd.read_csv(csv_path).sort_values("rate")
    if not components:
        raise ValueError(f"No CSV files found in {csv_dir}")
    return components


def plot_metric(components: dict, metric: str, ylabel: str, title: str, out_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(9, 5))
    for label, df in components.items():
        if metric not in df.columns:
            continue
        ax.plot(df["rate"], df[metric], marker="o", linewidth=1.8, markersize=4, label=label)

    ax.set_xlabel("rate (target requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel(ylabel, fontsize=11, labelpad=8)
    ax.set_title(title, fontsize=13, fontweight="bold", pad=10)
    ax.grid(True, linestyle="--", alpha=0.3)
    ax.legend(frameon=False, fontsize=9, ncol=2)
    fig.tight_layout()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot per-component pidstat CPU/RAM usage across a rate sweep."
    )
    parser.add_argument("--mode", choices=["vanilla", "proto"], required=True, help="Gateway mode")
    parser.add_argument("--pidstat-dir", default=None, help="Override path to the pidstat/ directory")
    parser.add_argument("--out-dir", default=None, help="Output directory for figures")
    args = parser.parse_args()

    base_dir = Path(__file__).resolve().parent
    mode_dir = MODE_DIR[args.mode]
    pidstat_dir = Path(args.pidstat_dir) if args.pidstat_dir else base_dir / "pidstat" / mode_dir
    out_dir = Path(args.out_dir) if args.out_dir else base_dir / "plots" / f"pidstat_{args.mode}"

    cpu_dir = pidstat_dir / "cpu"
    ram_dir = pidstat_dir / "ram"
    if not cpu_dir.is_dir() or not ram_dir.is_dir():
        print(f"ERROR: expected {cpu_dir} and {ram_dir} to exist", file=sys.stderr)
        return 1

    print(f"[plot] Loading CPU components from: {cpu_dir}")
    cpu_components = load_components(cpu_dir)
    print(f"[plot] Loading RAM components from: {ram_dir}")
    ram_components = load_components(ram_dir)

    mode_title = "Vanilla" if args.mode == "vanilla" else "SCM_RIGHTS Prototype"

    for metric, ylabel in CPU_METRICS:
        out_path = out_dir / f"cpu_{metric}.png"
        plot_metric(cpu_components, metric, ylabel, f"{mode_title}: {ylabel} per component", out_path)
        print(f"  wrote {out_path}")

    for metric, ylabel in RAM_METRICS:
        out_path = out_dir / f"ram_{metric}.png"
        plot_metric(ram_components, metric, ylabel, f"{mode_title}: {ylabel} per component", out_path)
        print(f"  wrote {out_path}")

    print(f"[ok] Wrote {len(CPU_METRICS) + len(RAM_METRICS)} figures in: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
