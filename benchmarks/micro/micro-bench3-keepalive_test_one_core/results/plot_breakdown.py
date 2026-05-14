"""
Breakdown bar plot: Vanilla vs Prototype
For each payload size:
  - Vanilla group (3 bars): delta_ns | delta_decrypt_ns | delta_migration_ns
  - Proto  group (3 bars): delta_ns | delta_migration_ns | worker_time (top2-top_container_before_read)
"""
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

VANILLA_CSV = "combined_vanilla_two_containers_alpha0_32kb_to_1024kb_rpc50_20260514_154710.csv"
PROTO_CSV   = "combined_proto_two_containers_alpha0_32kb_to_1024kb_rpc50_20260514_153944.csv"

vanilla = pd.read_csv(VANILLA_CSV)
proto   = pd.read_csv(PROTO_CSV)

# Derived column for proto: time spent inside the worker after migration
proto["worker_time_ns"] = proto["top2_rdtsc"] - proto["top_container_before_read"]

# Per-payload stats (in ms)
def stat(df, col, agg):
    grouped = df.groupby("payload_size")[col]
    if agg == "median":
        return grouped.median() / 1e6
    if agg == "mean":
        return grouped.mean() / 1e6
    raise ValueError(f"Unsupported agg: {agg}")

payloads    = sorted(vanilla["payload_size"].unique())
payloads_kb = [p / 1024 for p in payloads]
x = np.arange(len(payloads))

# -- colours ---------------------------------------------------------
# High-contrast, colorblind-friendly palette for clearer separation.
C_V_TOTAL   = "#000000"   # black   – vanilla total (top2-top1)
C_V_DECRYPT = "#D55E00"   # vermilion – vanilla TLS decrypt
C_V_MIGR    = "#F0E442"   # yellow  – vanilla migration to container

C_P_TOTAL   = "#0072B2"   # blue    – proto total (top2-top1)
C_P_MIGR    = "#009E73"   # green   – proto sendfd migration
C_P_WORKER  = "#CC79A7"   # magenta – proto worker processing

# -- group geometry --------------------------------------------------
# 6 bars per payload with a small gap between the two mode groups
W  = 0.15          # individual bar width (thicker bars)
GAP = 0.08         # gap between vanilla and proto sub-groups
# offsets from group centre
off = [-2.5*W - GAP/2, -1.5*W - GAP/2, -0.5*W - GAP/2,
        0.5*W + GAP/2,  1.5*W + GAP/2,  2.5*W + GAP/2]

def make_plot(agg):
    v_total   = stat(vanilla, "delta_ns", agg).reindex(payloads).values
    v_decrypt = stat(vanilla, "delta_decrypt_ns", agg).reindex(payloads).values
    v_migr    = stat(vanilla, "delta_migration_ns", agg).reindex(payloads).values
    p_total   = stat(proto,   "delta_ns", agg).reindex(payloads).values
    p_migr    = stat(proto,   "delta_migration_ns", agg).reindex(payloads).values
    p_worker  = stat(proto,   "worker_time_ns", agg).reindex(payloads).values

    series = [
        (v_total,   C_V_TOTAL,   "Vanilla  top2−top1 (total)"),
        (v_decrypt, C_V_DECRYPT, "Vanilla  delta_decrypt_ns"),
        (v_migr,    C_V_MIGR,    "Vanilla  delta_migration_ns"),
        (p_total,   C_P_TOTAL,   "Proto    top2−top1 (total)"),
        (p_migr,    C_P_MIGR,    "Proto    delta_migration_ns"),
        (p_worker,  C_P_WORKER,  "Proto    top2−top_container_before_read"),
    ]

    fig, ax = plt.subplots(figsize=(14, 6))

    for i, (values, color, label) in enumerate(series):
        positions = x + off[i]
        ax.bar(positions, values, W, color=color, label=label, edgecolor="white", linewidth=0.4)

    # Overlay total curves above bars (same y-axis in ms).
    ax.plot(
        x + off[0],
        v_total,
        color=C_V_TOTAL,
        marker="o",
        linewidth=2.0,
        markersize=4,
        label="Vanilla curve  top2−top1",
        zorder=4,
    )
    ax.plot(
        x + off[3],
        p_total,
        color=C_P_TOTAL,
        marker="s",
        linewidth=2.0,
        markersize=4,
        label="Proto curve  top2−top1",
        zorder=4,
    )

    # vertical separator between vanilla / proto sub-groups
    for xi in x:
        ax.axvline(xi, color="grey", linewidth=0.4, linestyle="--", alpha=0.4)

    ax.set_xlabel("Payload size (KB)", fontsize=11)
    ax.set_ylabel(f"{agg.capitalize()} latency (ms)", fontsize=11)
    ax.set_xticks(x)
    ax.set_xticklabels([f"{p:.0f}" for p in payloads_kb], rotation=45, ha="right")
    ax.legend(loc="upper left", fontsize=8, framealpha=0.9)
    ax.set_title(f"Latency breakdown — Vanilla vs Prototype (2 containers, α=0) [{agg}]", fontsize=12)
    plt.tight_layout()

    out = f"plot_breakdown_{agg}.png"
    plt.savefig(out, dpi=150)
    print(f"Saved: {out}")
    plt.close(fig)


make_plot("median")
make_plot("mean")

# Keep a default file name pointing to median for convenience.
Path("plot_breakdown.png").write_bytes(Path("plot_breakdown_median.png").read_bytes())
print("Saved: plot_breakdown.png")
