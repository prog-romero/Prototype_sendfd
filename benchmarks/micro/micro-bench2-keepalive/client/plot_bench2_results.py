#!/usr/bin/env python3
"""
plot_bench2_results.py - Comparison plots for benchmark-2 keepalive runs.

The script expects one vanilla CSV and one prototype CSV produced by
run_combined_sweep.py. It generates:
  - an overall ECDF on a log latency axis
  - payload/alpha/request-type latency profiles using medians and IQR bands
  - observed speedup heatmaps from matched median latencies
  - switched-request penalty profiles

If no input paths are provided, it looks for ../results/combined_vanilla.csv
and the newest ../results/combined_proto*.csv relative to this script.
"""

from __future__ import annotations

import argparse
import sys
import textwrap
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
    import seaborn as sns
    from matplotlib.colors import Normalize, TwoSlopeNorm
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
    "request_seq",
    "request_no",
    "worker",
    "target_fn",
    "switched",
    "delta_cycles",
    "cntfrq",
    "delta_ns",
    "requests_per_conn",
    "top1_rdtsc",
    "top2_rdtsc",
}

MODE_ORDER = ["vanilla", "prototype"]
TRAFFIC_ORDER = ["Same function", "Switched function"]
TRAFFIC_LABELS = {0: "Same function", 1: "Switched function"}
PALETTE = {"vanilla": "#5C6B8A", "prototype": "#D97757"}
MARKERS = {"vanilla": "o", "prototype": "s"}
FIGURE_FACE = "#F6F2EA"
AXES_FACE = "#FCFBF7"
GRID_COLOR = "#D9D1C5"
TEXT_COLOR = "#2E2924"


def q25(series: pd.Series) -> float:
    return float(series.quantile(0.25))


def q75(series: pd.Series) -> float:
    return float(series.quantile(0.75))


def q95(series: pd.Series) -> float:
    return float(series.quantile(0.95))


def human_bytes(value: int) -> str:
    if value >= 1024 * 1024 and value % (1024 * 1024) == 0:
        return f"{value // (1024 * 1024)} MiB"
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024} KiB"
    return f"{value} B"


def format_ratio(value: float) -> str:
    if pd.isna(value):
        return ""
    if value >= 100:
        return f"{value:.0f}x"
    if value >= 10:
        return f"{value:.1f}x"
    return f"{value:.2f}x"


def configure_style() -> None:
    sns.set_theme(
        style="whitegrid",
        context="talk",
        rc={
            "figure.facecolor": FIGURE_FACE,
            "axes.facecolor": AXES_FACE,
            "axes.edgecolor": "#A79B8B",
            "axes.labelcolor": TEXT_COLOR,
            "axes.titlecolor": TEXT_COLOR,
            "grid.color": GRID_COLOR,
            "grid.alpha": 0.45,
            "text.color": TEXT_COLOR,
            "xtick.color": "#554D44",
            "ytick.color": "#554D44",
            "font.size": 12,
        },
    )


def default_paths() -> tuple[Path | None, Path | None, Path]:
    script_dir = Path(__file__).resolve().parent
    results_dir = script_dir.parent / "results"
    vanilla = results_dir / "combined_vanilla.csv"
    proto_candidates = sorted(results_dir.glob("combined_proto*.csv"))
    proto = max(proto_candidates, key=lambda path: path.stat().st_mtime) if proto_candidates else None
    return vanilla if vanilla.exists() else None, proto, results_dir / "plots_comparison"


def resolve_path(path_value: str | None, fallback: Path | None, label: str) -> Path:
    path = Path(path_value).expanduser() if path_value else fallback
    if path is None or not path.exists():
        raise SystemExit(f"missing {label} CSV; pass --{label} or place it in the default results directory")
    return path.resolve()


def load_run(path: Path, mode: str) -> pd.DataFrame:
    frame = pd.read_csv(path)
    missing = sorted(REQUIRED_COLUMNS - set(frame.columns))
    if missing:
        raise SystemExit(f"{path} is missing required columns: {', '.join(missing)}")

    frame = frame.copy()
    frame["mode"] = mode
    frame["alpha"] = frame["alpha"].astype(int)
    frame["payload_size"] = frame["payload_size"].astype(int)
    frame["switched"] = frame["switched"].astype(int)
    frame["requests_per_conn"] = frame["requests_per_conn"].astype(int)
    frame["delta_ns"] = pd.to_numeric(frame["delta_ns"], errors="coerce")
    frame = frame.dropna(subset=["delta_ns"])
    frame["delta_ms"] = frame["delta_ns"] / 1_000_000.0
    frame["traffic"] = frame["switched"].map(TRAFFIC_LABELS)
    frame["payload_label"] = frame["payload_size"].map(human_bytes)
    return frame


def compare_runs(vanilla: pd.DataFrame, prototype: pd.DataFrame) -> list[str]:
    warnings: list[str] = []
    group_cols = ["alpha", "payload_size", "switched"]
    vanilla_counts = vanilla.groupby(group_cols).size()
    proto_counts = prototype.groupby(group_cols).size()

    vanilla_keys = set(vanilla_counts.index.tolist())
    proto_keys = set(proto_counts.index.tolist())
    if vanilla_keys != proto_keys:
        only_vanilla = sorted(vanilla_keys - proto_keys)[:5]
        only_proto = sorted(proto_keys - vanilla_keys)[:5]
        raise SystemExit(
            "the provided CSVs do not cover the same (alpha, payload_size, switched) cells; "
            f"vanilla-only={only_vanilla}, proto-only={only_proto}"
        )

    mismatches = [
        (key, int(vanilla_counts[key]), int(proto_counts[key]))
        for key in vanilla_counts.index
        if int(vanilla_counts[key]) != int(proto_counts[key])
    ]
    if mismatches:
        preview = ", ".join(f"{key}: {left} vs {right}" for key, left, right in mismatches[:4])
        warnings.append(f"sample counts differ in some matched cells ({preview}).")

    vanilla_rpc = sorted(vanilla["requests_per_conn"].unique().tolist())
    proto_rpc = sorted(prototype["requests_per_conn"].unique().tolist())
    if vanilla_rpc != proto_rpc:
        warnings.append(
            "requests_per_conn differs between files "
            f"(vanilla={vanilla_rpc}, prototype={proto_rpc}), so cross-mode speedups reflect the provided runs rather than identical TLS reuse."
        )

    return warnings


def build_group_summary(df: pd.DataFrame) -> pd.DataFrame:
    summary = df.groupby(
        ["mode", "alpha", "payload_size", "traffic", "requests_per_conn"],
        as_index=False,
    ).agg(
        samples=("delta_ms", "size"),
        median_ms=("delta_ms", "median"),
        p25_ms=("delta_ms", q25),
        p75_ms=("delta_ms", q75),
        p95_ms=("delta_ms", q95),
        mean_ms=("delta_ms", "mean"),
    )
    return summary.sort_values(["traffic", "alpha", "payload_size", "mode"])


def build_speedup_table(summary: pd.DataFrame) -> pd.DataFrame:
    table = summary.pivot_table(
        index=["alpha", "payload_size", "traffic"],
        columns="mode",
        values="median_ms",
    ).reset_index()
    table.columns.name = None
    table["speedup"] = np.where(
        (table["prototype"] > 0) & (table["vanilla"] > 0),
        table["vanilla"] / table["prototype"],
        np.nan,
    )
    return table.sort_values(["traffic", "alpha", "payload_size"])


def build_penalty_table(summary: pd.DataFrame) -> pd.DataFrame:
    table = summary.pivot_table(
        index=["mode", "alpha", "payload_size"],
        columns="traffic",
        values="median_ms",
    ).reset_index()
    table.columns.name = None
    for traffic in TRAFFIC_ORDER:
        if traffic not in table:
            table[traffic] = np.nan
    table["switched_penalty"] = table["Switched function"] / table["Same function"]
    return table.sort_values(["alpha", "payload_size", "mode"])


def export_tables(
    summary: pd.DataFrame,
    speedup: pd.DataFrame,
    penalty: pd.DataFrame,
    out_dir: Path,
) -> list[Path]:
    outputs = [
        out_dir / "bench2_latency_summary.csv",
        out_dir / "bench2_speedup_summary.csv",
        out_dir / "bench2_switched_penalty_summary.csv",
    ]
    summary.to_csv(outputs[0], index=False)
    speedup.to_csv(outputs[1], index=False)
    penalty.to_csv(outputs[2], index=False)
    return outputs


def style_axes(ax: plt.Axes) -> None:
    ax.set_facecolor(AXES_FACE)
    ax.grid(True, axis="y", alpha=0.45)
    ax.grid(False, axis="x")
    for spine in ax.spines.values():
        spine.set_color("#B0A493")


def add_footer(fig: plt.Figure, note: str) -> None:
    fig.text(
        0.01,
        0.01,
        textwrap.fill(note, width=150),
        ha="left",
        va="bottom",
        fontsize=8.5,
        color="#5B5248",
    )


def save_figure(fig: plt.Figure, out_dir: Path, stem: str) -> list[Path]:
    pdf_path = out_dir / f"{stem}.pdf"
    png_path = out_dir / f"{stem}.png"
    fig.savefig(pdf_path, bbox_inches="tight")
    fig.savefig(png_path, bbox_inches="tight", dpi=220)
    plt.close(fig)
    print(f"[plot] {stem} -> {pdf_path}")
    return [pdf_path, png_path]


def plot_overall_ecdf(
    df: pd.DataFrame,
    out_dir: Path,
    title_prefix: str,
    note: str,
) -> list[Path]:
    fig, ax = plt.subplots(figsize=(11, 6.4))
    stat_lines: list[str] = []

    for mode in MODE_ORDER:
        subset = df[df["mode"] == mode].sort_values("delta_ms")
        sns.ecdfplot(
            data=subset,
            x="delta_ms",
            ax=ax,
            linewidth=2.8,
            color=PALETTE[mode],
            label=mode.title(),
        )
        median_ms = float(subset["delta_ms"].median())
        p95_ms = float(subset["delta_ms"].quantile(0.95))
        rpc = ", ".join(str(value) for value in sorted(subset["requests_per_conn"].unique()))
        ax.axvline(median_ms, color=PALETTE[mode], linestyle="--", linewidth=1.6, alpha=0.85)
        ax.scatter([p95_ms], [0.95], color=PALETTE[mode], s=72, edgecolor="white", linewidth=0.8, zorder=6)
        stat_lines.append(f"{mode.title()}: median {median_ms:.3f} ms | p95 {p95_ms:.3f} ms | rpc {rpc}")

    style_axes(ax)
    ax.set_xscale("log")
    ax.set_xlabel("Latency (ms, log scale)")
    ax.set_ylabel("ECDF")
    fig.suptitle(f"{title_prefix}: overall latency distribution", x=0.06, ha="left", fontweight="bold")
    fig.text(
        0.06,
        0.92,
        "All requests combined. Dashed lines mark medians and dots mark p95.",
        ha="left",
        va="top",
        fontsize=11,
        color="#5B5248",
    )
    ax.legend(frameon=True, facecolor="white", edgecolor="#D5CCBF")
    ax.text(
        0.02,
        0.96,
        "\n".join(stat_lines),
        transform=ax.transAxes,
        ha="left",
        va="top",
        fontsize=10.5,
        bbox={"facecolor": "white", "alpha": 0.92, "edgecolor": "#D5CCBF", "boxstyle": "round,pad=0.4"},
    )
    fig.subplots_adjust(top=0.84, bottom=0.17)
    add_footer(fig, note)
    return save_figure(fig, out_dir, "bench2_overall_ecdf")


def plot_payload_profiles(
    summary: pd.DataFrame,
    out_dir: Path,
    title_prefix: str,
    note: str,
) -> list[Path]:
    payloads = sorted(summary["payload_size"].unique())
    payload_labels = [human_bytes(value) for value in payloads]
    alphas = sorted(summary["alpha"].unique())
    x = np.arange(len(payloads))

    fig, axes = plt.subplots(2, len(alphas), figsize=(18, 9.2), sharex=True, sharey="row")
    for row_index, traffic in enumerate(TRAFFIC_ORDER):
        for col_index, alpha in enumerate(alphas):
            ax = axes[row_index, col_index]
            panel = summary[(summary["traffic"] == traffic) & (summary["alpha"] == alpha)]

            for mode in MODE_ORDER:
                line = panel[panel["mode"] == mode].set_index("payload_size").reindex(payloads)
                median = line["median_ms"].to_numpy(dtype=float)
                p25 = line["p25_ms"].to_numpy(dtype=float)
                p75 = line["p75_ms"].to_numpy(dtype=float)
                ax.plot(
                    x,
                    median,
                    color=PALETTE[mode],
                    marker=MARKERS[mode],
                    linewidth=2.4,
                    markersize=6,
                    label=mode.title(),
                )
                ax.fill_between(
                    x,
                    p25,
                    p75,
                    where=np.isfinite(p25) & np.isfinite(p75),
                    color=PALETTE[mode],
                    alpha=0.16,
                )

            style_axes(ax)
            ax.set_yscale("log")
            ax.set_title(f"alpha = {alpha}", fontweight="bold")
            if col_index == 0:
                ax.set_ylabel(f"{traffic}\nLatency (ms)")
            if row_index == len(TRAFFIC_ORDER) - 1:
                ax.set_xticks(x, payload_labels, rotation=35, ha="right")
            else:
                ax.set_xticks(x, [])

    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper right",
        bbox_to_anchor=(0.96, 0.995),
        ncol=2,
        frameon=True,
        edgecolor="#D5CCBF",
    )
    fig.suptitle(f"{title_prefix}: latency by payload, locality, and request type", x=0.06, ha="left", fontweight="bold")
    fig.text(
        0.06,
        0.93,
        "Lines show medians; shaded bands show the interquartile range for each matched cell.",
        ha="left",
        va="top",
        fontsize=11,
        color="#5B5248",
    )
    fig.subplots_adjust(top=0.84, bottom=0.16, wspace=0.12, hspace=0.22)
    add_footer(fig, note)
    return save_figure(fig, out_dir, "bench2_payload_profiles")


def build_heatmap_norm(values: pd.Series) -> tuple[Normalize, object]:
    finite = values.replace([np.inf, -np.inf], np.nan).dropna()
    if finite.empty:
        return Normalize(vmin=0.0, vmax=1.0), sns.light_palette(PALETTE["prototype"], as_cmap=True)
    if finite.min() < 0 < finite.max():
        return (
            TwoSlopeNorm(vmin=float(finite.min()), vcenter=0.0, vmax=float(finite.max())),
            sns.diverging_palette(15, 145, s=85, l=45, as_cmap=True),
        )
    return (
        Normalize(vmin=float(finite.min()), vmax=float(finite.max())),
        sns.light_palette(PALETTE["prototype"], as_cmap=True),
    )


def plot_speedup_heatmap(
    speedup: pd.DataFrame,
    out_dir: Path,
    title_prefix: str,
    note: str,
) -> list[Path]:
    payloads = sorted(speedup["payload_size"].unique())
    payload_labels = [human_bytes(value) for value in payloads]
    alphas = sorted(speedup["alpha"].unique())
    fig, axes = plt.subplots(1, len(TRAFFIC_ORDER), figsize=(18, 5.8), sharey=True)
    log_speedup = np.log2(speedup["speedup"])
    norm, cmap = build_heatmap_norm(log_speedup)

    for index, traffic in enumerate(TRAFFIC_ORDER):
        ax = axes[index]
        panel = speedup[speedup["traffic"] == traffic]
        ratio_grid = panel.pivot(index="alpha", columns="payload_size", values="speedup").reindex(index=alphas, columns=payloads)
        annot_grid = ratio_grid.apply(lambda column: column.map(format_ratio))
        sns.heatmap(
            np.log2(ratio_grid),
            ax=ax,
            cmap=cmap,
            norm=norm,
            annot=annot_grid,
            fmt="",
            mask=ratio_grid.isna(),
            linewidths=0.8,
            linecolor=FIGURE_FACE,
            cbar=index == len(TRAFFIC_ORDER) - 1,
            cbar_kws={"label": "log2(vanilla / prototype median)"},
            annot_kws={"fontsize": 9.5},
        )
        ax.set_title(traffic, fontweight="bold")
        ax.set_xlabel("")
        ax.set_xticklabels(payload_labels, rotation=35, ha="right")
        ax.set_yticklabels([str(alpha) for alpha in alphas], rotation=0)
        if index == 0:
            ax.set_ylabel("alpha")
        else:
            ax.set_ylabel("")

    fig.suptitle(f"{title_prefix}: observed prototype speedup over vanilla", x=0.06, ha="left", fontweight="bold")
    fig.text(
        0.06,
        0.92,
        "Each cell compares median latency on the matched (alpha, payload size, request type) slice. Higher is better for prototype.",
        ha="left",
        va="top",
        fontsize=11,
        color="#5B5248",
    )
    fig.subplots_adjust(top=0.82, bottom=0.2, wspace=0.08)
    add_footer(fig, note)
    return save_figure(fig, out_dir, "bench2_speedup_heatmap")


def plot_switched_penalty(
    penalty: pd.DataFrame,
    out_dir: Path,
    title_prefix: str,
    note: str,
) -> list[Path]:
    payloads = sorted(penalty["payload_size"].unique())
    payload_labels = [human_bytes(value) for value in payloads]
    alphas = sorted(penalty["alpha"].unique())
    x = np.arange(len(payloads))

    fig, axes = plt.subplots(1, len(alphas), figsize=(18, 5.8), sharey=True)
    for index, alpha in enumerate(alphas):
        ax = axes[index]
        panel = penalty[penalty["alpha"] == alpha]
        for mode in MODE_ORDER:
            line = panel[panel["mode"] == mode].set_index("payload_size").reindex(payloads)
            ax.plot(
                x,
                line["switched_penalty"].to_numpy(dtype=float),
                color=PALETTE[mode],
                marker=MARKERS[mode],
                linewidth=2.4,
                markersize=6,
                label=mode.title(),
            )

        style_axes(ax)
        ax.set_title(f"alpha = {alpha}", fontweight="bold")
        ax.set_xticks(x, payload_labels, rotation=35, ha="right")
        ax.set_yscale("log")
        ax.axhline(1.0, color="#6A6156", linestyle="--", linewidth=1.3)
        if index == 0:
            ax.set_ylabel("Switched / same median latency")

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper right",
        bbox_to_anchor=(0.96, 0.995),
        ncol=2,
        frameon=True,
        edgecolor="#D5CCBF",
    )
    fig.suptitle(f"{title_prefix}: switched-request penalty by locality", x=0.06, ha="left", fontweight="bold")
    fig.text(
        0.06,
        0.93,
        "Values above 1 mean switched requests are slower. Missing points indicate slices where the schedule produced only one request type.",
        ha="left",
        va="top",
        fontsize=11,
        color="#5B5248",
    )
    fig.subplots_adjust(top=0.84, bottom=0.2, wspace=0.1)
    add_footer(fig, note)
    return save_figure(fig, out_dir, "bench2_switched_penalty")


def build_note(vanilla: pd.DataFrame, prototype: pd.DataFrame, warnings: list[str]) -> str:
    vanilla_rpc = ",".join(str(value) for value in sorted(vanilla["requests_per_conn"].unique()))
    prototype_rpc = ",".join(str(value) for value in sorted(prototype["requests_per_conn"].unique()))
    base = (
        f"Matched cells: {len(vanilla)} rows/mode, {vanilla['alpha'].nunique()} alphas, "
        f"{vanilla['payload_size'].nunique()} payload sizes."
    )
    if not warnings:
        return f"{base} rpc matched across both files."
    return f"{base} Fairness note: rpc differs (vanilla={vanilla_rpc}, prototype={prototype_rpc})."


def write_notes(
    out_dir: Path,
    vanilla_path: Path,
    proto_path: Path,
    note: str,
) -> Path:
    note_path = out_dir / "bench2_plot_notes.txt"
    note_path.write_text(
        "\n".join(
            [
                f"vanilla_csv={vanilla_path}",
                f"prototype_csv={proto_path}",
                note,
                "details=Matched (alpha, payload_size, switched) coverage; interpret cross-mode speedups with the rpc mismatch in mind.",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    return note_path


def parse_args() -> argparse.Namespace:
    default_vanilla, default_proto, default_out_dir = default_paths()
    parser = argparse.ArgumentParser(description="Generate comparison plots for benchmark-2 keepalive runs")
    parser.add_argument("--vanilla", default=str(default_vanilla) if default_vanilla else None)
    parser.add_argument("--proto", default=str(default_proto) if default_proto else None)
    parser.add_argument("--out-dir", default=str(default_out_dir))
    parser.add_argument("--title-prefix", default="Benchmark 2 Keepalive")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    configure_style()

    default_vanilla, default_proto, _ = default_paths()
    vanilla_path = resolve_path(args.vanilla, default_vanilla, "vanilla")
    proto_path = resolve_path(args.proto, default_proto, "proto")
    out_dir = Path(args.out_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    vanilla = load_run(vanilla_path, "vanilla")
    prototype = load_run(proto_path, "prototype")
    warnings = compare_runs(vanilla, prototype)
    note = build_note(vanilla, prototype, warnings)

    combined = pd.concat([vanilla, prototype], ignore_index=True)
    summary = build_group_summary(combined)
    speedup = build_speedup_table(summary)
    penalty = build_penalty_table(summary)

    exported = export_tables(summary, speedup, penalty, out_dir)
    note_path = write_notes(out_dir, vanilla_path, proto_path, note)

    generated: list[Path] = []
    generated.extend(plot_overall_ecdf(combined, out_dir, args.title_prefix, note))
    generated.extend(plot_payload_profiles(summary, out_dir, args.title_prefix, note))
    generated.extend(plot_speedup_heatmap(speedup, out_dir, args.title_prefix, note))
    generated.extend(plot_switched_penalty(penalty, out_dir, args.title_prefix, note))

    print("[plot] summaries ->")
    for path in [*exported, note_path, *generated]:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
