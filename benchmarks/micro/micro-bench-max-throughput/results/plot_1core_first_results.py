#!/usr/bin/env python3
import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
VANILLA_CSV = os.path.join(BASE_DIR, "vanilla_alt_1core_32kb_v2.csv")
PROTO_CSV = os.path.join(BASE_DIR, "proto_alt_1core_32kb_v2.csv")
MAX_CONCURRENCY = 211

vanilla = pd.read_csv(VANILLA_CSV)
proto = pd.read_csv(PROTO_CSV)

vanilla = vanilla[vanilla["concurrency"] <= MAX_CONCURRENCY].copy()
proto = proto[proto["concurrency"] <= MAX_CONCURRENCY].copy()

# Keep only common concurrency points.
common = sorted(set(vanilla["concurrency"]).intersection(set(proto["concurrency"])))
vanilla = vanilla[vanilla["concurrency"].isin(common)].sort_values("concurrency")
proto = proto[proto["concurrency"].isin(common)].sort_values("concurrency")

x = np.arange(len(common))
labels = [str(c) for c in common]
width = 0.38


def annotate_bars(ax, bars, fmt="{:.2f}", fontsize=6, dy=1.0):
    for bar in bars:
        h = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            h + dy,
            fmt.format(h),
            ha="center",
            va="bottom",
            fontsize=fontsize,
            rotation=90,
        )

# ---- Figure 1: Throughput bar chart ----
fig, ax = plt.subplots(figsize=(13, 5.5))

bars_v = ax.bar(x - width / 2, vanilla["transfer_kb_s"], width=width, label="Vanilla", color="#1f77b4")
bars_p = ax.bar(x + width / 2, proto["transfer_kb_s"], width=width, label="Prototype", color="#ff7f0e")

annotate_bars(ax, bars_v, fmt="{:.2f}", fontsize=6, dy=0.35)
annotate_bars(ax, bars_p, fmt="{:.2f}", fontsize=6, dy=0.35)

ax.set_title("Throughput Comparison (1 Core, up to Concurrency 211)")
ax.set_xlabel("Concurrency")
ax.set_ylabel("Throughput (KB/s)")
ax.set_xticks(x)
ax.set_xticklabels(labels, rotation=45)
ax.legend()
ax.grid(axis="y", alpha=0.25)
plt.tight_layout()

out1 = os.path.join(BASE_DIR, "first_result_1core_throughput_bar.png")
fig.savefig(out1, dpi=180)
plt.close(fig)

# ---- Figure 2: Total requests comparison + tiny error labels ----
fig, ax = plt.subplots(figsize=(13, 5.5))

bars_v = ax.bar(x - width / 2, vanilla["total_requests"], width=width, label="Vanilla", color="#1f77b4")
bars_p = ax.bar(x + width / 2, proto["total_requests"], width=width, label="Prototype", color="#ff7f0e")

for bar, err in zip(bars_v, vanilla["errors_non2xx"]):
    x0 = bar.get_x() + bar.get_width() / 2
    y0 = bar.get_height() + 75
    ax.text(x0, y0, f"e:{int(err)}", fontsize=6, color="#1f77b4", ha="center", va="bottom")

for bar, err in zip(bars_p, proto["errors_non2xx"]):
    x0 = bar.get_x() + bar.get_width() / 2
    y0 = bar.get_height() + 75
    ax.text(x0, y0, f"e:{int(err)}", fontsize=6, color="#ff7f0e", ha="center", va="bottom")

ax.set_title("Total Requests Comparison (1 Core, up to Concurrency 211)")
ax.set_xlabel("Concurrency")
ax.set_ylabel("Total requests in step")
ax.set_xticks(x)
ax.set_xticklabels(labels, rotation=45)
ax.legend()
ax.grid(alpha=0.25)
plt.tight_layout()

out2 = os.path.join(BASE_DIR, "first_result_1core_requests_compare.png")
fig.savefig(out2, dpi=180)
plt.close(fig)

# ---- Figure 3: Average latency comparison ----
fig, ax = plt.subplots(figsize=(13, 5.5))

bars_v = ax.bar(x - width / 2, vanilla["lat_avg_ms"], width=width, label="Vanilla", color="#1f77b4")
bars_p = ax.bar(x + width / 2, proto["lat_avg_ms"], width=width, label="Prototype", color="#ff7f0e")

annotate_bars(ax, bars_v, fmt="{:.1f}", fontsize=6, dy=2.2)
annotate_bars(ax, bars_p, fmt="{:.1f}", fontsize=6, dy=2.2)

ax.set_title("Average Latency Comparison (1 Core, up to Concurrency 211)")
ax.set_xlabel("Concurrency")
ax.set_ylabel("Average latency (ms)")
ax.set_xticks(x)
ax.set_xticklabels(labels, rotation=45)
ax.legend()
ax.grid(alpha=0.25)
plt.tight_layout()

out3 = os.path.join(BASE_DIR, "first_result_1core_latency_avg_compare.png")
fig.savefig(out3, dpi=180)
plt.close(fig)

print("Generated:")
print(out1)
print(out2)
print(out3)
