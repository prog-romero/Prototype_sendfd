import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

def load_and_process(path):
    df = pd.read_csv(path)
    # Group by payload_bytes and calculate mean latency in ms
    summary = df.groupby('payload_bytes')['delta_ns'].mean() / 1e6
    return summary.reset_index()

def plot_range(v_data, p_data, start_kb, end_kb, filename):
    # Filter data for the specific range
    mask_v = (v_data['payload_bytes'] >= start_kb * 1024) & (v_data['payload_bytes'] <= end_kb * 1024)
    mask_p = (p_data['payload_bytes'] >= start_kb * 1024) & (p_data['payload_bytes'] <= end_kb * 1024)
    
    v_subset = v_data[mask_v].copy()
    p_subset = p_data[mask_p].copy()
    
    # Merge to ensure alignment
    df = pd.merge(v_subset, p_subset, on='payload_bytes', suffixes=('_vanilla', '_proto'))
    df['size_kb'] = df['payload_bytes'] / 1024
    df['gain_pct'] = ((df['delta_ns_vanilla'] - df['delta_ns_proto']) / df['delta_ns_vanilla']) * 100

    fig, ax1 = plt.subplots(figsize=(12, 6))

    x = np.arange(len(df))
    width = 0.35

    # Plot bars for mean latency
    v_bars = ax1.bar(x - width/2, df['delta_ns_vanilla'], width, label='Vanilla HTTP', color='#ff9999', alpha=0.8)
    p_bars = ax1.bar(x + width/2, df['delta_ns_proto'], width, label='Proto Zero-Copy', color='#66b3ff', alpha=0.8)
    
    # Add trend curves for latency only
    ax1.plot(x - width/2, df['delta_ns_vanilla'], color='red', marker='.', linestyle='--', linewidth=0.5, alpha=0.3)
    ax1.plot(x + width/2, df['delta_ns_proto'], color='blue', marker='.', linestyle='--', linewidth=0.5, alpha=0.3)

    # Add gain text on top of the proto bars
    for i, bar in enumerate(p_bars):
        gain = df['gain_pct'].iloc[i]
        ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5, 
                 f'{gain:.1f}%', ha='center', va='bottom', fontsize=8, fontweight='bold', color='green', rotation=90)

    ax1.set_xlabel('Payload Size (KB)')
    ax1.set_ylabel('Mean time (ms)')
    ax1.set_xticks(x)
    ax1.set_xticklabels([f"{int(s)}" for s in df['size_kb']], rotation=45, style='italic')
    ax1.legend(loc='upper left')
    ax1.grid(axis='y', linestyle=':', alpha=0.6)

    plt.title(f'Time Comparison: {start_kb}KB - {end_kb}KB')
    plt.tight_layout()
    plt.savefig(filename)
    print(f"Saved plot: {filename}")
    plt.close()

def main():
    results_dir = 'benchmarks/micro/micro-bench2-initial-http/results'
    vanilla_path = os.path.join(results_dir, 'vanilla_http_2mb.csv')
    proto_path = os.path.join(results_dir, 'proto_http_2mb.csv')

    if not os.path.exists(vanilla_path) or not os.path.exists(proto_path):
        print("Error: Result files not found.")
        return

    v_data = load_and_process(vanilla_path)
    p_data = load_and_process(proto_path)

    # Three plot ranges
    plot_range(v_data, p_data, 32, 1000, os.path.join(results_dir, 'plot_32_1000kb.png'))
    plot_range(v_data, p_data, 1000, 1500, os.path.join(results_dir, 'plot_1000_1500kb.png'))
    plot_range(v_data, p_data, 1500, 2048, os.path.join(results_dir, 'plot_1500_2048kb.png'))

if __name__ == "__main__":
    main()
