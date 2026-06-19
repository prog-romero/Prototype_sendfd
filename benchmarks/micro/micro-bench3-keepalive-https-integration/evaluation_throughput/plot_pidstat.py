#!/usr/bin/env python3
"""
plot_pidstat.py — Plot per-component CPU and RAM usage across a rate sweep.

Reads pidstat/<mode>/{cpu,ram}/<component>.csv (one row per rate step, as
produced by sweep_throughput_wrk2.py) and plots each metric as one figure,
with one line per component.

All figures within the same metric group share the exact same Y-axis scale
and tick graduation so they are directly comparable at a glance:
  - CPU group  (usr_pct, system_pct, cpu_pct)  — same Y scale, same ticks
  - RSS group  (rss_kb)                         — own scale
  - mem% group (mem_pct)                        — own scale

Use --ymax-cpu, --ymax-rss, --ymax-mem to pin the Y limits across two runs
(e.g. vanilla vs proto) so cross-mode plots are comparable too.

Usage:
    python3 plot_pidstat.py --mode proto
    python3 plot_pidstat.py --mode vanilla --out-dir plots/pidstat_vanilla
    # force same limits as a previous proto run for cross-mode comparison:
    python3 plot_pidstat.py --mode vanilla --ymax-cpu 120 --ymax-rss 300000 --ymax-mem 8
"""

import argparse
import math
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import pandas as pd
except ImportError:
    print(
        "ERROR: matplotlib and pandas are required.  "
        "Install with: pip3 install matplotlib pandas",
        file=sys.stderr,
    )
    sys.exit(1)

# sweep_throughput_wrk2.py / collect_pidstat.sh use "prototype" as the on-disk
# folder name for --mode proto.
MODE_DIR = {"vanilla": "vanilla", "proto": "prototype"}

CPU_METRICS = [
    ("usr_pct",    "user CPU (%)"),
    ("system_pct", "system CPU (%)"),
    ("cpu_pct",    "total CPU (%)"),
]
RAM_METRICS = [
    ("rss_kb",  "RSS memory (KB)"),
    ("mem_pct", "memory (%)"),
]


# ── helpers ────────────────────────────────────────────────────────────────────

def load_components(csv_dir: Path) -> dict:
    components = {}
    for csv_path in sorted(csv_dir.glob("*.csv")):
        components[csv_path.stem] = pd.read_csv(csv_path).sort_values("rate")
    if not components:
        raise ValueError(f"No CSV files found in {csv_dir}")
    return components


def group_data_max(components: dict, metric_names: list) -> float:
    """Return the max value across all components and all listed metric columns."""
    vmax = 0.0
    for df in components.values():
        for m in metric_names:
            if m in df.columns:
                col_max = df[m].max()
                if not math.isnan(col_max):
                    vmax = max(vmax, col_max)
    return vmax


_NICE_STEPS = [
    0.1, 0.2, 0.5,
    1, 2, 5, 10, 20, 50, 100, 200, 500,
    1_000, 2_000, 5_000, 10_000, 20_000, 50_000,
    100_000, 200_000, 500_000, 1_000_000,
]


def nice_ylim_and_step(data_max: float, forced_max: float | None = None) -> tuple:
    """
    Return (ymax, tick_step) so all plots in a group share the exact same Y axis.

    ymax  — forced_max if given, otherwise data_max + 10 % headroom rounded up
            to the nearest step boundary (always a clean round number).
    step  — the largest value from _NICE_STEPS that gives between 5 and 12
            evenly-spaced labelled ticks, for readability without clutter.

    Examples (auto mode):
        12.3   → ymax=  14, step=  2  → ticks 0,2,4,…,14   (7 ticks)
        44.08  → ymax=  50, step= 10  → ticks 0,10,…,50     (5 ticks)
        87.4   → ymax= 100, step= 10  → ticks 0,10,…,100    (10 ticks)
        110.0  → ymax= 140, step= 20  → ticks 0,20,…,140    (7 ticks)
        222870 → ymax=250000,step=50k → ticks 0,50k,…,250k  (5 ticks)
        5.74   → ymax=   7, step=  1  → ticks 0,1,…,7       (7 ticks)
    """
    if data_max <= 0:
        data_max = 1.0

    if forced_max is None:
        headroom = data_max * 1.10
        for step in _NICE_STEPS:
            ymax = math.ceil(headroom / step) * step
            n = round(ymax / step)
            if 5 <= n <= 12:
                return float(ymax), float(step)
        # Fallback: use the largest step
        step = _NICE_STEPS[-1]
        ymax = math.ceil(headroom / step) * step
        return float(ymax), float(step)

    # Forced ymax: pick the step that gives the most ticks while staying ≤ 12.
    ymax = float(forced_max)
    for step in reversed(_NICE_STEPS):
        n = ymax / step
        if 5 <= n <= 12:
            return ymax, float(step)
    return ymax, ymax / 5


def plot_metric(
    components: dict,
    metric: str,
    ylabel: str,
    title: str,
    out_path: Path,
    ylim: float,
    tick_step: float,
) -> None:
    fig, ax = plt.subplots(figsize=(9, 5))

    for label, df in components.items():
        if metric not in df.columns:
            continue
        ax.plot(
            df["rate"], df[metric],
            marker="o", linewidth=1.8, markersize=4, label=label,
        )

    ax.set_xlabel("rate (target requests/sec)", fontsize=11, labelpad=8)
    ax.set_ylabel(ylabel, fontsize=11, labelpad=8)
    ax.set_title(title, fontsize=13, fontweight="bold", pad=10)

    # ── shared, uniform Y axis ──────────────────────────────────────────────
    ax.set_ylim(0, ylim)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(tick_step))
    ax.yaxis.set_minor_locator(ticker.AutoMinorLocator(2))   # half-step minor ticks
    ax.grid(True, which="major", linestyle="--", alpha=0.35)
    ax.grid(True, which="minor", linestyle=":",  alpha=0.15)
    # ─────────────────────────────────────────────────────────────────────────

    ax.legend(frameon=False, fontsize=9, ncol=2)
    fig.tight_layout()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


# ── main ───────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot per-component pidstat CPU/RAM usage across a rate sweep."
    )
    parser.add_argument(
        "--mode", choices=["vanilla", "proto"], required=True, help="Gateway mode"
    )
    parser.add_argument(
        "--pidstat-dir", default=None,
        help="Override path to the pidstat/ directory",
    )
    parser.add_argument(
        "--out-dir", default=None,
        help="Output directory for figures",
    )
    # Optional forced Y limits — use these to get identical axes across two
    # separate runs (e.g. vanilla and proto) for direct visual comparison.
    parser.add_argument(
        "--ymax-cpu", type=float, default=None,
        help="Force Y-axis max for all CPU figures (usr/system/total %%)",
    )
    parser.add_argument(
        "--ymax-rss", type=float, default=None,
        help="Force Y-axis max for the RSS figure (KB)",
    )
    parser.add_argument(
        "--ymax-mem", type=float, default=None,
        help="Force Y-axis max for the memory %% figure",
    )
    args = parser.parse_args()

    base_dir  = Path(__file__).resolve().parent
    mode_dir  = MODE_DIR[args.mode]
    pidstat_dir = (
        Path(args.pidstat_dir) if args.pidstat_dir
        else base_dir / "pidstat" / mode_dir
    )
    out_dir = (
        Path(args.out_dir) if args.out_dir
        else base_dir / "plots" / f"pidstat_{args.mode}"
    )

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

    # ── compute GLOBAL scales from ALL available modes ─────────────────────
    # Scan every mode directory (vanilla + prototype) that has data on disk so
    # that the Y limits are the same regardless of which mode you plot.
    # If --ymax-* is given it overrides the auto-computed global max.
    global_cpu_max = 0.0
    global_rss_max = 0.0
    global_mem_max = 0.0

    for mode_subdir in MODE_DIR.values():
        scan_base = base_dir / "pidstat"
        scan_cpu = scan_base / mode_subdir / "cpu"
        scan_ram = scan_base / mode_subdir / "ram"
        if scan_cpu.is_dir():
            try:
                c = load_components(scan_cpu)
                global_cpu_max = max(global_cpu_max,
                                     group_data_max(c, ["usr_pct", "system_pct", "cpu_pct"]))
            except ValueError:
                pass
        if scan_ram.is_dir():
            try:
                r = load_components(scan_ram)
                global_rss_max = max(global_rss_max, group_data_max(r, ["rss_kb"]))
                global_mem_max = max(global_mem_max, group_data_max(r, ["mem_pct"]))
            except ValueError:
                pass

    cpu_ylim, cpu_step = nice_ylim_and_step(global_cpu_max, args.ymax_cpu)
    rss_ylim, rss_step = nice_ylim_and_step(global_rss_max, args.ymax_rss)
    mem_ylim, mem_step = nice_ylim_and_step(global_mem_max, args.ymax_mem)

    print(
        f"  Y scales —  CPU: [0, {cpu_ylim:.0f}] step={cpu_step:.0f}  |  "
        f"RSS: [0, {rss_ylim:.0f}] step={rss_step:.0f}  |  "
        f"mem%: [0, {mem_ylim:.1f}] step={mem_step:.1f}"
    )
    # ───────────────────────────────────────────────────────────────────────

    for metric, ylabel in CPU_METRICS:
        out_path = out_dir / f"cpu_{metric}.png"
        plot_metric(
            cpu_components, metric, ylabel,
            f"{mode_title}: {ylabel} per component",
            out_path, ylim=cpu_ylim, tick_step=cpu_step,
        )
        print(f"  wrote {out_path}")

    for metric, ylabel in RAM_METRICS:
        out_path = out_dir / f"ram_{metric}.png"
        ylim, step = (rss_ylim, rss_step) if metric == "rss_kb" else (mem_ylim, mem_step)
        plot_metric(
            ram_components, metric, ylabel,
            f"{mode_title}: {ylabel} per component",
            out_path, ylim=ylim, tick_step=step,
        )
        print(f"  wrote {out_path}")

    print(f"[ok] Wrote {len(CPU_METRICS) + len(RAM_METRICS)} figures in: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
