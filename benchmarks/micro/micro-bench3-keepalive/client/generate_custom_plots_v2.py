import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# --- Configuration ---
RESULTS_DIR = '/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive/results'
OUTPUT_DIR = os.path.join(RESULTS_DIR, 'final_plots_v2')
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Data files
FILES = {
    'one_container': {
        'vanilla': 'combined_vanilla_one_container_alpha0_32kb_to_1024kb_rpc50_20260426_114211.csv',
        'proto':   'combined_proto_one_container_alpha0_32kb_to_1024kb_rpc50_20260424_181421.csv'
    },
    'two_containers': {
        'vanilla': 'combined_vanilla_two_containers_alpha0_32kb_to_1024kb_rpc50_20260426_143258.csv',
        'proto':   'combined_proto_two_containers_alpha0_32kb_to_1024kb_rpc50_20260424_154140.csv'
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
    ax.set_title(f'Latency Comparison: {metric_name.upper()}{title_suffix}', 
                 fontsize=18, pad=25, fontweight='bold', color='#2C3E50')
    ax.set_xlabel('Payload Size (KiB)', fontsize=13, labelpad=15)
    ax.set_ylabel('Latency (ms)', fontsize=13, labelpad=15)
    
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
    for case, tools in FILES.items():
        print(f"Processing {case}...")
        
        try:
            v_df = pd.read_csv(os.path.join(RESULTS_DIR, tools['vanilla']))
            p_df = pd.read_csv(os.path.join(RESULTS_DIR, tools['proto']))
        except FileNotFoundError as e:
            print(f"  Error: {e}")
            continue
            
        v_stats = get_stats(v_df)
        p_stats = get_stats(p_df)
        
        # Plotting Mean and Median as requested
        for metric in ['mean', 'median']:
            path = plot_metric(case, metric, v_stats, p_stats)
            print(f"  Generated {metric} plot: {path}")

if __name__ == "__main__":
    main()
