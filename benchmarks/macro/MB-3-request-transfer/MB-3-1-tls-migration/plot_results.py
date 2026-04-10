#!/usr/bin/env python3

"""
plot_results.py — Generate comprehensive visualizations for MB-3.1 benchmark

Reads from:
  - results/mb3_1_results.csv
  - results/mb3_1_gateway_timings.csv
  - results/mb3_1_worker_timings.csv
  - results/mb3_1_handshake_timings.csv

Generates:
  - Box plot: PATH A vs PATH B
  - Histogram: Distribution plots
  - Statistical comparison table
"""

import csv
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
import statistics as stats
import os

def read_timings(filename):
    """Read single-column timing CSV file"""
    values = []
    try:
        with open(filename, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        values.append(float(line))
                    except ValueError:
                        pass
    except FileNotFoundError:
        print(f"Warning: {filename} not found")
    return values

# Read individual timing files
gateway_times = read_timings('results/mb3_1_gateway_timings.csv')
worker_times = read_timings('results/mb3_1_worker_timings.csv')
handshake_times = read_timings('results/mb3_1_handshake_timings.csv')

# Calculate migration totals
migration_times = [g + w for g, w in zip(gateway_times, worker_times)]

if not migration_times or not handshake_times:
    print("ERROR: No data to plot")
    exit(1)

print(f"[plot] Loaded {len(migration_times)} migration measurements")
print(f"[plot] Loaded {len(handshake_times)} handshake measurements")

# Create figure with subplots
fig = plt.figure(figsize=(15, 5))

# ─────────────────────────────────────────────────────────────────────────── #
# Plot 1: Box Plot Comparison
# ─────────────────────────────────────────────────────────────────────────── #

ax1 = plt.subplot(1, 3, 1)

bp = ax1.boxplot([migration_times, handshake_times], 
                   labels=['TLS State\nMigration (µs)', 'Fresh TLS 1.3\nHandshake (µs)'],
                   patch_artist=True,
                   showmeans=True,
                   meanline=False,
                   widths=0.6)

# Style the box plot
colors = ['#2ecc71', '#e74c3c']
for patch, color in zip(bp['boxes'], colors):
    patch.set_facecolor(color)
    patch.set_alpha(0.7)

for whisker in bp['whiskers']:
    whisker.set(linewidth=1.5)
for cap in bp['caps']:
    cap.set(linewidth=1.5)
for median in bp['medians']:
    median.set(color='darkred', linewidth=2)
for mean in bp['means']:
    mean.set(marker='D', markerfacecolor='blue', markeredgecolor='darkblue', markersize=8)

ax1.set_ylabel('Time', fontsize=11, fontweight='bold')
ax1.set_title('TLS Migration vs Fresh Handshake', fontsize=12, fontweight='bold')
ax1.grid(True, axis='y', alpha=0.3)

# Add legend for mean marker
ax1.plot([], [], marker='D', linestyle='', color='blue', markersize=8, label='Mean')
ax1.plot([], [], color='darkred', linewidth=2, label='Median')
ax1.legend(loc='upper left', fontsize=9)

# ─────────────────────────────────────────────────────────────────────────── #
# Plot 2: Histogram - Migration Times
# ─────────────────────────────────────────────────────────────────────────── #

ax2 = plt.subplot(1, 3, 2)

ax2.hist(migration_times, bins=20, color='#2ecc71', alpha=0.7, edgecolor='black', linewidth=1.2)
ax2.axvline(stats.mean(migration_times), color='darkblue', linestyle='--', linewidth=2, label=f'Mean: {stats.mean(migration_times):.1f} µs')
ax2.axvline(stats.median(migration_times), color='darkred', linestyle='-', linewidth=2, label=f'Median: {stats.median(migration_times):.1f} µs')

ax2.set_xlabel('Time', fontsize=10, fontweight='bold')
ax2.set_ylabel('Frequency', fontsize=10, fontweight='bold')
ax2.set_title('TLS Migration Time Distribution', fontsize=11, fontweight='bold')
ax2.legend(fontsize=9)
ax2.grid(True, axis='y', alpha=0.3)

# ─────────────────────────────────────────────────────────────────────────── #
# Plot 3: Histogram - Handshake Times
# ─────────────────────────────────────────────────────────────────────────── #

ax3 = plt.subplot(1, 3, 3)

ax3.hist(handshake_times, bins=20, color='#e74c3c', alpha=0.7, edgecolor='black', linewidth=1.2)
ax3.axvline(stats.mean(handshake_times), color='darkblue', linestyle='--', linewidth=2, label=f'Mean: {stats.mean(handshake_times):.1f} µs')
ax3.axvline(stats.median(handshake_times), color='darkred', linestyle='-', linewidth=2, label=f'Median: {stats.median(handshake_times):.1f} µs')

ax3.set_xlabel('Time', fontsize=10, fontweight='bold')
ax3.set_ylabel('Frequency', fontsize=10, fontweight='bold')
ax3.set_title('Fresh Handshake Time Distribution', fontsize=11, fontweight='bold')
ax3.legend(fontsize=9)
ax3.grid(True, axis='y', alpha=0.3)

plt.tight_layout()

# Save plots
plot_png = 'results/mb3_1_comparison.png'
plot_pdf = 'results/mb3_1_comparison.pdf'

try:
    plt.savefig(plot_png, dpi=150, bbox_inches='tight')
    print(f"\n[✓] Saved PNG plot to: {plot_png}")
except Exception as e:
    print(f"ERROR saving PNG: {e}")

try:
    plt.savefig(plot_pdf, bbox_inches='tight')
    print(f"[✓] Saved PDF plot to: {plot_pdf}")
except Exception as e:
    print(f"ERROR saving PDF: {e}")

# Display summary statistics
print("\n" + "="*80)
print("STATISTICAL SUMMARY")
print("="*80 + "\n")

print("TLS STATE MIGRATION")
print("-" * 80)
print(f"  Count:      {len(migration_times)}")
print(f"  Mean:       {stats.mean(migration_times):>10.2f} µs")
print(f"  Median:     {stats.median(migration_times):>10.2f} µs")
print(f"  Std Dev:    {stats.stdev(migration_times):>10.2f} µs")
print(f"  Min/Max:    {min(migration_times):>10.2f} / {max(migration_times):<10.2f} µs\n")

print("FRESH TLS 1.3 HANDSHAKE")
print("-" * 80)
print(f"  Count:      {len(handshake_times)}")
print(f"  Mean:       {stats.mean(handshake_times):>10.2f} µs ({stats.mean(handshake_times)/1000:.3f} ms)")
print(f"  Median:     {stats.median(handshake_times):>10.2f} µs ({stats.median(handshake_times)/1000:.3f} ms)")
print(f"  Std Dev:    {stats.stdev(handshake_times):>10.2f} µs")
print(f"  Min/Max:    {min(handshake_times):>10.2f} / {max(handshake_times):<10.2f} µs\n")

print("SPEEDUP ANALYSIS")
print("-" * 80)
speedup_mean = stats.mean(handshake_times) / stats.mean(migration_times)
speedup_median = stats.median(handshake_times) / stats.median(migration_times)
time_saved = stats.mean(handshake_times) - stats.mean(migration_times)

print(f"  Speedup (mean):     {speedup_mean:>8.1f}x")
print(f"  Speedup (median):   {speedup_median:>8.1f}x")
print(f"  Time saved (mean):  {time_saved:>8.2f} µs ({time_saved/1000:.3f} ms per request)\n")

print("="*80)
