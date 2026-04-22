#!/usr/bin/env python3
"""Create alpha-specific comparison figures for bench2 keepalive runs.

This script expects four CSVs:
    - vanilla alpha 0
    - prototype alpha 0
    - vanilla alpha 100
    - prototype alpha 100

For each alpha it generates one figure with a single panel:
    - mean time as bars
    - p99 time as dashed curves
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
    import seaborn as sns
    from matplotlib.ticker import FuncFormatter
except ImportError:
    print(
        "pandas, matplotlib, seaborn, and numpy are required: "
        "pip install pandas matplotlib seaborn numpy",
        file=sys.stderr,
    )
    raise SystemExit(1)


REQUIRED_COLUMNS = {
    "label",
    "alpha",
    "payload_size",
    "delta_ns",
    "requests_per_conn",
}

PAYLOAD_ORDER = [64, 256, 1024, 4096, 16384, 65536, 131072, 262144, 524288, 1048576]
MODE_ORDER = ["vanilla", "prototype"]
MODE_LABELS = {"vanilla": "Vanilla", "prototype": "Prototype"}
COLORS = {"vanilla": "#4E5D78", "prototype": "#D56C45"}
FACE = "#F7F4EE"
AXFACE = "#FFFEFB"
GRID = "#D9D2C8"
TEXT = "#2F2924"


def human_bytes(value: int) -> str:
    if value >= 1024 * 1024 and value % (1024 * 1024) == 0:
        return f"{value // (1024 * 1024)} MiB"
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024} KiB"
    return f"{value} B"


def plain_ms(value: float) -> str:
    if value >= 1000:
        return f"{value:,.0f}"
    if value >= 100:
        return f"{value:,.1f}"
    if value >= 10:
        return f"{value:,.2f}"
    if value >= 1:
        return f"{value:,.3f}"
    return f"{value:,.4f}"


def ytick_formatter(value: float, _: int) -> str:
    if value <= 0:
        return "0"
    return plain_ms(value)


def configure_style() -> None:
    sns.set_theme(
        style="whitegrid",
        context="talk",
        rc={
            "figure.facecolor": FACE,
            "axes.facecolor": AXFACE,
            "axes.edgecolor": "#A89D8B",
            "axes.labelcolor": TEXT,
            "axes.titlecolor": TEXT,
            "xtick.color": "#5A5148",
            "ytick.color": "#5A5148",
            "grid.color": GRID,
            "grid.alpha": 0.45,
            "text.color": TEXT,
        },
    )


def latest_match(results_dir: Path, pattern: str) -> Path | None:
    matches = sorted(results_dir.glob(pattern), key=lambda path: path.stat().st_mtime)
    return matches[-1] if matches else None


def resolve_paths(results_dir: Path, args: argparse.Namespace) -> dict[int, dict[str, Path]]:
    mapping = {
        0: {
            "vanilla": Path(args.vanilla_alpha0).resolve() if args.vanilla_alpha0 else latest_match(results_dir, "combined_vanilla_alpha0_rpc50_npp100_*.csv"),
            "prototype": Path(args.proto_alpha0).resolve() if args.proto_alpha0 else latest_match(results_dir, "proto_alpha0_uniform1000_rpc50_*.csv"),
        },
        100: {
            "vanilla": Path(args.vanilla_alpha100).resolve() if args.vanilla_alpha100 else latest_match(results_dir, "combined_vanilla_alpha100_rpc50_npp100_*.csv"),
            "prototype": Path(args.proto_alpha100).resolve() if args.proto_alpha100 else latest_match(results_dir, "proto_alpha100_uniform1000_rpc50_*.csv"),
        },
    }

    missing: list[str] = []
    for alpha, mode_paths in mapping.items():
        for mode, path in mode_paths.items():
            if path is None or not path.exists():
                missing.append(f"alpha={alpha} {mode}")
    if missing:
        raise SystemExit(f"missing required CSVs for: {', '.join(missing)}")
    return mapping  # type: ignore[return-value]


def load_run(path: Path, alpha: int, mode: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    missing = sorted(REQUIRED_COLUMNS - set(df.columns))
    if missing:
        raise SystemExit(f"{path} is missing required columns: {', '.join(missing)}")

    if sorted(df["alpha"].dropna().astype(int).unique().tolist()) != [alpha]:
        raise SystemExit(f"{path} does not contain only alpha={alpha} rows")

    df = df.copy()
    df["mode"] = mode
    df["delta_ms"] = pd.to_numeric(df["delta_ns"], errors="coerce") / 1_000_000.0
    df = df.dropna(subset=["delta_ms"])
    df["payload_size"] = df["payload_size"].astype(int)
    df["payload_label"] = pd.Categorical(
        df["payload_size"].map(human_bytes),
        categories=[human_bytes(value) for value in PAYLOAD_ORDER],
        ordered=True,
    )
    return df


def build_summary(df: pd.DataFrame) -> pd.DataFrame:
    summary = df.groupby(["mode", "payload_size"], as_index=False).agg(
        mean_ms=("delta_ms", "mean"),
        p99_ms=("delta_ms", lambda series: float(series.quantile(0.99))),
        samples=("delta_ms", "size"),
    )
    return summary.sort_values(["payload_size", "mode"])


def style_axis(ax: plt.Axes) -> None:
    ax.set_facecolor(AXFACE)
    ax.grid(True, axis="y", alpha=0.45)
    ax.grid(False, axis="x")
    for spine in ax.spines.values():
        spine.set_color("#AA9F8D")


def alpha_title(alpha: int) -> str:
    if alpha == 0:
        return "Keepalive with different container function for different request."
    if alpha == 100:
        return "Keepalive with same container function for all request."
    return f"Keepalive alpha {alpha}"


def plot_alpha_figure(alpha: int, df: pd.DataFrame, out_dir: Path) -> list[Path]:
    payload_labels = [human_bytes(value) for value in PAYLOAD_ORDER]
    summary = build_summary(df)

    fig, ax = plt.subplots(figsize=(15, 7.5))

    x = np.arange(len(PAYLOAD_ORDER))
    width = 0.34

    for mode, shift in [("vanilla", -width / 2), ("prototype", width / 2)]:
        mode_summary = summary[summary["mode"] == mode].set_index("payload_size").reindex(PAYLOAD_ORDER)
        means = mode_summary["mean_ms"].to_numpy(dtype=float)
        p99s = mode_summary["p99_ms"].to_numpy(dtype=float)
        xpos = x + shift

        ax.bar(
            xpos,
            means,
            width=width,
            color=COLORS[mode],
            alpha=0.35,
            edgecolor=COLORS[mode],
            linewidth=1.2,
            label=f"{MODE_LABELS[mode]} mean",
        )
        ax.plot(
            xpos,
            p99s,
            color=COLORS[mode],
            linestyle="--",
            marker="o",
            linewidth=2.2,
            markersize=5.5,
            label=f"{MODE_LABELS[mode]} p99",
        )

    style_axis(ax)
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(ytick_formatter))
    ax.set_xticks(x, payload_labels, rotation=35, ha="right")
    ax.set_xlabel("Payload size")
    ax.set_ylabel("Time (ms)")
    ax.set_title(alpha_title(alpha), fontweight="bold")
    ax.legend(
        ncol=2,
        frameon=True,
        facecolor="white",
        edgecolor="#D5CEC4",
        loc="upper left",
    )

    fig.subplots_adjust(top=0.9, bottom=0.2, left=0.08, right=0.98)
    stem = f"bench2_alpha{alpha}_mean_p99"
    pdf = out_dir / f"{stem}.pdf"
    png = out_dir / f"{stem}.png"
    fig.savefig(pdf, bbox_inches="tight")
    fig.savefig(png, bbox_inches="tight", dpi=220)
    plt.close(fig)
    print(f"[plot] alpha {alpha} -> {pdf}")
    return [pdf, png]


def export_summary_table(frames: list[pd.DataFrame], out_dir: Path) -> Path:
    full = pd.concat(frames, ignore_index=True)
    summary = full.groupby(["alpha", "mode", "payload_size"], as_index=False).agg(
        mean_ms=("delta_ms", "mean"),
        p99_ms=("delta_ms", lambda series: float(series.quantile(0.99))),
        samples=("delta_ms", "size"),
    )
    summary_path = out_dir / "bench2_alpha_summary.csv"
    summary.to_csv(summary_path, index=False)
    return summary_path


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    results_dir = script_dir.parent / "results"
    parser = argparse.ArgumentParser(description="Create alpha=0 and alpha=100 comparison figures")
    parser.add_argument("--results-dir", default=str(results_dir))
    parser.add_argument("--vanilla-alpha0", default=None)
    parser.add_argument("--proto-alpha0", default=None)
    parser.add_argument("--vanilla-alpha100", default=None)
    parser.add_argument("--proto-alpha100", default=None)
    parser.add_argument("--out-dir", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    configure_style()

    results_dir = Path(args.results_dir).expanduser().resolve()
    out_dir = Path(args.out_dir).expanduser().resolve() if args.out_dir else results_dir / "plots_alpha_compare"
    out_dir.mkdir(parents=True, exist_ok=True)

    paths = resolve_paths(results_dir, args)

    frames: list[pd.DataFrame] = []
    generated: list[Path] = []
    for alpha in [0, 100]:
        vanilla = load_run(paths[alpha]["vanilla"], alpha, "vanilla")
        prototype = load_run(paths[alpha]["prototype"], alpha, "prototype")
        combined = pd.concat([vanilla, prototype], ignore_index=True)
        frames.append(combined.assign(alpha=alpha))
        generated.extend(plot_alpha_figure(alpha, combined, out_dir))

    summary_path = export_summary_table(frames, out_dir)
    print("[plot] summary ->")
    print(f"  {summary_path}")
    for path in generated:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())