import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# --- Configuration ---
RESULTS_DIR = '/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive/results'
OUTPUT_DIR = os.path.join(RESULTS_DIR, 'final_plots')
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Data files (Latest identified in previous step)
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

COLORS = {
    'vanilla': '#5C6B8A',  # Sleek blue-gray
    'proto':   '#D97757'   # Warm terracotta
}

# --- Style Configuration ---
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.size': 10,
    'axes.grid': True,
    'grid.alpha': 0.3,
    'axes.edgecolor': '#cccccc',
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
    """Generates a figure with bars and curves for a specific metric."""
    v_data = vanilla_stats[metric_name]
    p_data = proto_stats[metric_name]
    payloads_kb = vanilla_stats['payloads_kb']
    
    x = np.arange(len(payloads_kb))
    width = 0.35
    
    fig, ax = plt.subplots(figsize=(12, 7))
    
    # Histogram (Bars)
    bar1 = ax.bar(x - width/2, p_data.values, width, label='Prototype (libtlspeek)', color=COLORS['proto'], alpha=0.8, edgecolor='white', linewidth=1)
    bar2 = ax.bar(x + width/2, v_data.values, width, label='Vanilla (Proxy)', color=COLORS['vanilla'], alpha=0.8, edgecolor='white', linewidth=1)
    
    # Curves (Lines)
    ax.plot(x, p_data.values, color=COLORS['proto'], marker='o', markersize=6, linewidth=2, linestyle='-', markerfacecolor='white', markeredgewidth=2)
    ax.plot(x, v_data.values, color=COLORS['vanilla'], marker='s', markersize=6, linewidth=2, linestyle='-', markerfacecolor='white', markeredgewidth=2)
    
    # Value annotations on bars
    for bars in [bar1, bar2]:
        for bar in bars:
            height = bar.get_height()
            ax.annotate(f'{height:.2f}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 5),  # 5 points vertical offset
                        textcoords='offset points',
                        ha='center', va='bottom', fontsize=9, fontweight='bold', color='#444444')
    
    # Set labels and title
    title_suffix = " (One Container)" if case_name == "one_container" else " (Two Containers)"
    ax.set_title(f'Performance Comparison: {metric_name.capitalize()} Latency{title_suffix}', fontsize=16, pad=20, fontweight='bold')
    ax.set_xlabel('Payload Size (KiB)', fontsize=12, labelpad=10)
    ax.set_ylabel('Latency (ms)', fontsize=12, labelpad=10)
    
    ax.set_xticks(x)
    ax.set_xticklabels([f'{int(val)}' for val in payloads_kb], rotation=0)
    
    ax.legend(frameon=True, facecolor='white', framealpha=1, loc='upper left', fontsize=11)
    
    plt.tight_layout()
    
    # Save file
    filename = f"{case_name}_{metric_name}.png"
    filepath = os.path.join(OUTPUT_DIR, filename)
    plt.savefig(filepath, dpi=300)
    plt.close()
    return filepath

def main():
    for case, tools in FILES.items():
        print(f"Processing {case}...")
        
        # Load data
        v_df = pd.read_csv(os.path.join(RESULTS_DIR, tools['vanilla']))
        p_df = pd.read_csv(os.path.join(RESULTS_DIR, tools['proto']))
        
        v_stats = get_stats(v_df)
        p_stats = get_stats(p_df)
        
        for metric in ['mean', 'median', 'p95']:
            path = plot_metric(case, metric, v_stats, p_stats)
            print(f"  Generated {metric} plot: {path}")

if __name__ == "__main__":
    main()
