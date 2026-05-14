import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os
import glob

# --- Configuration ---
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, '..', 'results'))
OUTPUT_DIR = os.path.join(RESULTS_DIR, 'final_plots_v2')
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Data file patterns (latest matching file is selected)
FILE_PATTERNS = {
    'one_container': {
        'vanilla': 'combined_vanilla_one_container_alpha0_32kb_to_1024kb_rpc50_*.csv',
        'proto':   'combined_proto_one_container_alpha0_32kb_to_1024kb_rpc50_*.csv'
    },
    'two_containers': {
        'vanilla': 'combined_vanilla_two_containers_alpha0_32kb_to_1024kb_rpc50_*.csv',
        'proto':   'combined_proto_two_containers_alpha0_32kb_to_1024kb_rpc50_*.csv'
    }
}

# Updated Colors for better aesthetics
COLORS = {
    'proto':   '#E67E22',  # Vibrant Orange
    'vanilla': '#27AE60'   # Elegant Green
}

# --- Style Configuration ---
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.size': 11,
    'axes.labelsize': 12,
    'axes.titlesize': 15,
    'legend.fontsize': 11,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'axes.grid': True,
    'grid.alpha': 0.3,
    'axes.spines.top': False,
    'axes.spines.right': False,
})

def get_stats(df):
    """Computes mean, median, and p95 for each payload size."""
    grouped = df.groupby('payload_size')['delta_ns']
    mean = grouped.mean() / 1e6
    median = grouped.median() / 1e6
    p95 = grouped.quantile(0.95) / 1e6
    return {
        'mean': mean,
        'median': median,
        'p95': p95,
        'payloads_kb': mean.index / 1024
    }


def resolve_latest(pattern):
    """Return newest CSV matching pattern in RESULTS_DIR, else None."""
    matches = glob.glob(os.path.join(RESULTS_DIR, pattern))
    if not matches:
        return None
    return max(matches, key=os.path.getmtime)

def plot_metric(case_name, metric_name, vanilla_stats, proto_stats):
    """Generates a highly aesthetic figure with spaced bars and inclined labels."""
    v_data = vanilla_stats[metric_name]
    p_data = proto_stats[metric_name]
    payloads_kb = vanilla_stats['payloads_kb']
    
    x = np.arange(len(payloads_kb))
    width = 0.30       # Reduced width for better spacing
    spacing = 0.05     # Gap between bars in a group
    
    fig, ax = plt.subplots(figsize=(11, 7))
    
    # Histogram (Bars) - Spaced out manually
    # Note: subtracted spacing/2 and added spacing/2 to separate them
    bar1 = ax.bar(x - (width/2 + spacing/2), p_data.values, width, 
                  label='Prototype (libtlspeek)', color=COLORS['proto'], 
                  alpha=0.85, edgecolor='none', zorder=2)
    
    bar2 = ax.bar(x + (width/2 + spacing/2), v_data.values, width, 
                  label='Vanilla (Standard Proxy)', color=COLORS['vanilla'], 
                  alpha=0.85, edgecolor='none', zorder=2)
    
    # Curves (Lines) - Slightly offset markers to match bar centers
    ax.plot(x - (width/2 + spacing/2), p_data.values, color=COLORS['proto'], 
            marker='o', markersize=7, linewidth=2.5, linestyle='-', 
            markerfacecolor='white', markeredgewidth=2, zorder=3)
    
    ax.plot(x + (width/2 + spacing/2), v_data.values, color=COLORS['vanilla'], 
            marker='s', markersize=7, linewidth=2.5, linestyle='-', 
            markerfacecolor='white', markeredgewidth=2, zorder=3)
    
    # Set labels and title
    title_suffix = " (One Container)" if case_name == "one_container" else " (Two Containers)"
    ax.set_title(f'Performance Comparison: {metric_name.capitalize()} Time{title_suffix}', 
                 fontsize=18, pad=25, fontweight='bold', color='#2C3E50')
    ax.set_xlabel('Payload Size (KiB)', fontsize=13, labelpad=15)
    ax.set_ylabel('Time (ms)', fontsize=13, labelpad=15)
    
    # X-axis ticks and inclined labels (Inclined for readability)
    ax.set_xticks(x)
    ax.set_xticklabels([f'{int(val)} KiB' for val in payloads_kb], rotation=45, ha='right')
    
    # Legend
    ax.legend(frameon=True, facecolor='white', framealpha=0.9, loc='upper left', 
              fontsize=12, borderpad=1)
    
    # Aesthetic adjustments
    plt.grid(True, linestyle='--', alpha=0.5, zorder=1)
    plt.tight_layout()
    
    # Save file
    filename = f"{case_name}_{metric_name}_v2.png"
    filepath = os.path.join(OUTPUT_DIR, filename)
    plt.savefig(filepath, dpi=300)
    plt.close()
    return filepath

def main():
    print(f"Using RESULTS_DIR={RESULTS_DIR}")
    print(f"Writing OUTPUT_DIR={OUTPUT_DIR}")

    for case, tools in FILE_PATTERNS.items():
        print(f"Processing {case}...")

        vanilla_path = resolve_latest(tools['vanilla'])
        proto_path = resolve_latest(tools['proto'])

        if vanilla_path is None or proto_path is None:
            print(f"  Error: could not find both CSV files for {case}")
            print(f"    vanilla pattern: {tools['vanilla']}")
            print(f"    proto pattern:   {tools['proto']}")
            continue

        print(f"  vanilla CSV: {os.path.basename(vanilla_path)}")
        print(f"  proto CSV:   {os.path.basename(proto_path)}")

        v_df = pd.read_csv(vanilla_path)
        p_df = pd.read_csv(proto_path)
            
        v_stats = get_stats(v_df)
        p_stats = get_stats(p_df)
        
        # Plotting Mean and Median as requested
        for metric in ['mean', 'median']:
            path = plot_metric(case, metric, v_stats, p_stats)
            print(f"  Generated {metric} plot: {path}")

if __name__ == "__main__":
    main()
