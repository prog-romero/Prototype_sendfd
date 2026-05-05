import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# Results directory
results_dir = "../results"

def load_data(filename):
    path = os.path.join(results_dir, filename)
    if not os.path.exists(path):
        print(f"File not found: {path}")
        return pd.DataFrame()
    df = pd.read_csv(path)
    # Filter out errors and 0 delta_ns
    # Convert http_status to string to be safe
    df['http_status'] = df['http_status'].astype(str)
    df = df[df['http_status'] == '200']
    df = df[df['delta_ns'] > 0]
    print(f"Loaded {len(df)} rows from {filename}")
    # Convert delta_ns to ms
    df['latency_ms'] = df['delta_ns'] / 1e6
    return df

def get_stats(df):
    # Group by payload_bytes and calculate mean
    stats = df.groupby('payload_bytes')['latency_ms'].mean().reset_index()
    return stats

# Load all data
vanilla_single = get_stats(load_data("vanilla_single.csv"))
vanilla_switch = get_stats(load_data("vanilla_switch.csv"))
proto_single = get_stats(load_data("proto_single.csv"))
proto_switch = get_stats(load_data("proto_switch.csv"))

def plot_range(min_kb, max_kb, title, filename):
    # Filter data by range
    v_data = vanilla_switch[(vanilla_switch['payload_bytes'] >= min_kb * 1024) & (vanilla_switch['payload_bytes'] <= max_kb * 1024)]
    p_data = proto_switch[(proto_switch['payload_bytes'] >= min_kb * 1024) & (proto_switch['payload_bytes'] <= max_kb * 1024)]
    
    if v_data.empty or p_data.empty:
        print(f"No data for range {min_kb}-{max_kb} KB")
        return

    # Merge to ensure we have matching sizes
    merged = pd.merge(v_data, p_data, on='payload_bytes', suffixes=('_vanilla', '_proto'))
    
    labels = [f"{int(s/1024)}K" for s in merged['payload_bytes']]
    vanilla_means = merged['latency_ms_vanilla']
    proto_means = merged['latency_ms_proto']
    
    x = np.arange(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(14, 7))
    rects1 = ax.bar(x - width/2, vanilla_means, width, label='Vanilla (Proxy)', color='#E74C3C', alpha=0.8, edgecolor='black', linewidth=0.5)
    rects2 = ax.bar(x + width/2, proto_means, width, label='Proto (Relay)', color='#3498DB', alpha=0.8, edgecolor='black', linewidth=0.5)

    ax.set_ylabel('Mean Latency (ms)', fontsize=12, fontweight='bold')
    ax.set_title(title, fontsize=16, fontweight='bold', pad=20)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha='right', style='italic', fontsize=10)
    ax.legend(fontsize=12)
    ax.grid(axis='y', linestyle='--', alpha=0.7)

    # Add gain text on top of bars
    for i, (v, p) in enumerate(zip(vanilla_means, proto_means)):
        gain = ((v - p) / v) * 100
        ax.annotate(f'{gain:.1f}%',
                    xy=(x[i] + width/2, p),
                    xytext=(0, 5),  # 5 points vertical offset
                    textcoords="offset points",
                    ha='center', va='bottom', 
                    fontsize=9, fontweight='bold', 
                    color='#2C3E50',
                    rotation=90) # Vertical rotation for legibility

    fig.tight_layout()
    plt.savefig(filename, dpi=300)
    print(f"Saved {filename}")

# Generate the three plots
plot_range(32, 1000, "Keep-Alive Benchmark (32KB - 1000KB)", "ka_results_32_1000.png")
plot_range(1000, 1500, "Keep-Alive Benchmark (1000KB - 1500KB)", "ka_results_1000_1500.png")
plot_range(1500, 2000, "Keep-Alive Benchmark (1500KB - 2000KB)", "ka_results_1500_2000.png")
