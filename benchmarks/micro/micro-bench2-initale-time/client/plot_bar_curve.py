#!/usr/bin/env python3
"""
plot_bar_curve.py
=================
Generates 4 figures (2 metrics × 2 payload windows):

  - metric: mean time   → figures for 32-512 KiB and 512-1024 KiB
  - metric: P75 time    → figures for 32-512 KiB and 512-1024 KiB

Each figure contains:
  - Grouped bar chart  (Vanilla vs Prototype)
  - Curve overlay on   (Vanilla vs Prototype lines connecting bar tops)
  - Percentage gap annotation above every bar pair   (+ = proto faster)

Usage (from the repo root):
  python3 benchmarks/micro/micro-bench2-initale-time/client/plot_bar_curve.py
"""

import csv
import math
import os
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ─── Paths ────────────────────────────────────────────────────────────────────
BASE = Path(__file__).resolve().parent

VANILLA_CSV = BASE / "vanilla_results.csv"
PROTO_CSV   = BASE / "proto_results.csv"
OUT_DIR     = BASE

# ─── Colours ──────────────────────────────────────────────────────────────────
VANILLA_COLOR = "#1769aa"
PROTO_COLOR   = "#d9480f"
VANILLA_LIGHT = "#6aaed6"
PROTO_LIGHT   = "#f4956a"
GAP_COLOR     = "#2d6a2d"
ANN_COLOR     = "#333333"

# ─── Helpers ──────────────────────────────────────────────────────────────────

def ns_to_ms(v: float) -> float:
    return v / 1_000_000.0


def percentile(values, pct: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = (len(ordered) - 1) * pct / 100.0
    lo, hi = int(math.floor(rank)), int(math.ceil(rank))
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (1.0 - (rank - lo)) + ordered[hi] * (rank - lo)


def load_csv(path: Path) -> dict:
    """Returns {payload_bytes: [delta_ns, ...]}"""
    grouped = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row.get("http_status", "").strip() not in ("200",):
                continue
            try:
                payload = int(row["payload_bytes"])
                delta   = float(row["delta_ns"])
                if delta > 0:
                    grouped[payload].append(delta)
            except (KeyError, ValueError):
                continue
    return dict(grouped)


def summarize(grouped: dict, metric: str) -> dict:
    """Returns {payload_bytes: float_ms}"""
    out = {}
    for payload, vals in grouped.items():
        if not vals:
            continue
        if metric == "mean":
            out[payload] = ns_to_ms(float(np.mean(vals)))
        elif metric == "p75":
            out[payload] = ns_to_ms(percentile(vals, 75))
    return out


def filter_window(summary_v, summary_p, lo_kb: int, hi_kb: int):
    """Keep only payload sizes in [lo_kb, hi_kb] KiB, return aligned sorted arrays."""
    common = sorted(
        k for k in set(summary_v) & set(summary_p)
        if lo_kb * 1024 <= k <= hi_kb * 1024
    )
    return common, [summary_v[k] for k in common], [summary_p[k] for k in common]


def style_ax(ax):
    ax.grid(axis="y", linestyle="--", alpha=0.25, zorder=0)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


# ─── Core plot ────────────────────────────────────────────────────────────────

def make_figure(
    summary_v: dict,
    summary_p: dict,
    metric_label: str,
    lo_kb: int,
    hi_kb: int,
    out_path: Path,
):
    payloads, vals_v, vals_p = filter_window(summary_v, summary_p, lo_kb, hi_kb)
    if not payloads:
        print(f"  [skip] no data for {lo_kb}-{hi_kb} KiB")
        return

    n          = len(payloads)
    x          = np.arange(n)
    bar_width  = 0.36
    half       = bar_width / 2.0

    x_v_bars   = x - half
    x_p_bars   = x + half

    vals_v_arr = np.array(vals_v, dtype=float)
    vals_p_arr = np.array(vals_p, dtype=float)

    # x-tick labels: KiB
    labels = [str(p // 1024) for p in payloads]

    fig, ax = plt.subplots(figsize=(max(12, n * 0.6), 7))

    # ── Bars
    ax.bar(x_v_bars, vals_v_arr, width=bar_width, color=VANILLA_COLOR, alpha=0.82,
           label="Vanilla", zorder=3, edgecolor="white", linewidth=0.6)
    ax.bar(x_p_bars, vals_p_arr, width=bar_width, color=PROTO_COLOR, alpha=0.82,
           label="Prototype", zorder=3, edgecolor="white", linewidth=0.6)

    # ── Curves connecting bar tops (centre of each bar)
    ax.plot(x_v_bars, vals_v_arr, marker="o", markersize=5.5, linewidth=2.2,
            color=VANILLA_COLOR, zorder=4, label="_nolegend_")
    ax.plot(x_p_bars, vals_p_arr, marker="s", markersize=5.5, linewidth=2.2,
            color=PROTO_COLOR,  zorder=4, label="_nolegend_")

    # ── Percentage gap annotations above each pair
    for i, (vv, vp) in enumerate(zip(vals_v_arr, vals_p_arr)):
        if vv > 0:
            pct = (vv - vp) / vv * 100.0  # positive → proto is faster
        else:
            pct = 0.0

        top = max(vv, vp) * 1.04
        color = GAP_COLOR if pct > 0 else "#a00000"
        ax.text(
            x[i], top,
            f"{pct:+.0f}%",
            ha="center", va="bottom",
            fontsize=8.5, fontweight="bold",
            color=color, zorder=5,
        )

    # ── Axis decoration
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45 if n > 12 else 0, ha="right" if n > 12 else "center")
    ax.set_xlabel("Payload size (KiB)", fontsize=12)
    ax.set_ylabel("Time (ms)", fontsize=12)
    ax.set_title(
        f"{metric_label}  ·  Top1→Top2 time  ({lo_kb}–{hi_kb} KiB)\n"
        f"Green % = prototype faster   |   Red % = vanilla faster",
        fontsize=13, fontweight="bold",
    )

    style_ax(ax)

    # ── Legend: combine bars + curves into a clean legend
    van_bar  = mpatches.Patch(color=VANILLA_COLOR, alpha=0.82, label="Vanilla")
    pro_bar  = mpatches.Patch(color=PROTO_COLOR,   alpha=0.82, label="Prototype")
    van_line = plt.Line2D([], [], color=VANILLA_COLOR, marker="o", markersize=5, linewidth=2, label="Vanilla (curve)")
    pro_line = plt.Line2D([], [], color=PROTO_COLOR,   marker="s", markersize=5, linewidth=2, label="Prototype (curve)")
    ax.legend(handles=[van_bar, pro_bar, van_line, pro_line],
              frameon=False, loc="upper left", fontsize=10)

    fig.tight_layout()
    plt.savefig(str(out_path), dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"  [✓] saved → {out_path.name}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    print(f"Loading  {VANILLA_CSV.name} …")
    grouped_v = load_csv(VANILLA_CSV)
    print(f"Loading  {PROTO_CSV.name} …")
    grouped_p = load_csv(PROTO_CSV)

    for metric_key, metric_label in [("mean", "Mean time"), ("p75", "P75 time")]:
        print(f"\n── {metric_label} ──")
        summary_v = summarize(grouped_v, metric_key)
        summary_p = summarize(grouped_p, metric_key)

        for lo_kb, hi_kb in [(32, 512), (512, 1024)]:
            fname = f"bar_curve_{metric_key}_{lo_kb}to{hi_kb}KiB.png"
            make_figure(
                summary_v, summary_p,
                metric_label,
                lo_kb, hi_kb,
                OUT_DIR / fname,
            )

    print("\nDone.")


if __name__ == "__main__":
    main()
