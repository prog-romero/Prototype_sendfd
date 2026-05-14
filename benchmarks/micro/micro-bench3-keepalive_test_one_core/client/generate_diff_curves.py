import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# --- Configuration ---
RESULTS_DIR = '/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive/results'
OUTPUT_DIR = os.path.join(RESULTS_DIR, 'final_plots_diff')
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Latest identified Data files
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

COLOR_DIFF = '#9B59B6'  # Elegant Amethyst Purple for the difference curve

# --- Style Configuration ---
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.size': 11,
    'axes.labelsize': 12,
    'axes.titlesize': 15,
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
    return {
        'mean': mean,
        'median': median,
        'payloads_kb': mean.index / 1024
    }

def plot_difference(case_name, metric_name, vanilla_stats, proto_stats):
    """Generates a curve showing the latency difference (Prototype - Vanilla)."""
    v_data = vanilla_stats[metric_name]
    p_data = proto_stats[metric_name]
    diff = p_data - v_data
    payloads_kb = vanilla_stats['payloads_kb']
    
    x = np.arange(len(payloads_kb))
    
    fig, ax = plt.subplots(figsize=(11, 7))
    
    # Plot the difference curve
    ax.plot(x, diff.values, color=COLOR_DIFF, marker='D', markersize=8, 
            linewidth=3, linestyle='-', markerfacecolor='white', markeredgewidth=2,
            label=f'Difference ({metric_name.capitalize()})')
    
    # Zero line for reference
    ax.axhline(0, color='#34495E', linestyle='--', linewidth=1.5, alpha=0.7, label='Reference (Zero)')
    
    # Shade the area (Red if Proto > Vanilla, Green if Proto < Vanilla)
    ax.fill_between(x, diff.values, 0, where=(diff.values > 0), color='#E74C3C', alpha=0.1, interpolate=True)
    ax.fill_between(x, diff.values, 0, where=(diff.values < 0), color='#2ECC71', alpha=0.1, interpolate=True)
    
    # Set labels and title
    title_suffix = " (One Container)" if case_name == "one_container" else " (Two Containers)"
    ax.set_title(f'Time Difference: Prototype - Vanilla [{metric_name.upper()}]{title_suffix}', 
                 fontsize=16, pad=25, fontweight='bold', color='#2C3E50')
    ax.set_xlabel('Payload Size (KiB)', fontsize=13, labelpad=15)
    ax.set_ylabel('Time Delta (ms)', fontsize=13, labelpad=15)
    
    # X-axis ticks
    ax.set_xticks(x)
    ax.set_xticklabels([f'{int(val)} KiB' for val in payloads_kb], rotation=45, ha='right')
    
    # Annotation note
    text = "Negative value = Prototype is FASTER"
    ax.text(0.5, -0.15, text, transform=ax.transAxes, ha='center', fontsize=11, 
            fontweight='bold', color='#27AE60', bbox=dict(facecolor='white', alpha=0.8, edgecolor='#27AE60'))

    ax.legend(frameon=True, facecolor='white', framealpha=0.9, loc='upper left')
    
    plt.tight_layout()
    
    # Save file
    filename = f"diff_{case_name}_{metric_name}.png"
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
        
        # Plotting Difference for Mean and Median as requested
        for metric in ['mean', 'median']:
            path = plot_difference(case, metric, v_stats, p_stats)
            print(f"  Generated difference curve: {path}")

if __name__ == "__main__":
    main()
