
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Load CSVs
proto = pd.read_csv('benchmarks/micro/micro-bench3-keepalive/results/combined_proto_one_container_alpha0_32kb_to_1024kb_rpc50_20260424_181421.csv')
vanilla = pd.read_csv('benchmarks/micro/micro-bench3-keepalive/results/combined_vanilla_one_container_alpha0_32kb_to_1024kb_rpc50_20260426_114211.csv')

def summarize(df):
    grouped = df.groupby('payload_size')['delta_ns']
    mean = grouped.mean() / 1e6
    median = grouped.median() / 1e6
    return mean, median

mean_proto, median_proto = summarize(proto)
mean_vanilla, median_vanilla = summarize(vanilla)
payloads = mean_proto.index
payloads_kb = payloads / 1024
bar_width = 0.35
x = np.arange(len(payloads))

# Print payload sizes in KB
print("Payload sizes (KB):", [f"{p:.1f}" for p in payloads_kb])

# Plot mean only
fig, ax1 = plt.subplots(figsize=(10, 6))
bars1 = ax1.bar(x - bar_width/2, mean_proto.values, bar_width, label='Prototype', color='tab:blue')
bars2 = ax1.bar(x + bar_width/2, mean_vanilla.values, bar_width, label='Vanilla', color='tab:orange')

for bar in bars1:
    ax1.annotate(f'{bar.get_height():.1f}',
                 xy=(bar.get_x() + bar.get_width()/2, bar.get_height()),
                 xytext=(0, 3), textcoords='offset points',
                 ha='center', va='bottom', fontsize=8)
for bar in bars2:
    ax1.annotate(f'{bar.get_height():.1f}',
                 xy=(bar.get_x() + bar.get_width()/2, bar.get_height()),
                 xytext=(0, 3), textcoords='offset points',
                 ha='center', va='bottom', fontsize=8)


ax1.set_xlabel('Payload size (KB)')
ax1.set_ylabel('Mean time (ms)')
ax1.set_xticks(x)
ax1.set_xticklabels([f"{p:.1f}" for p in payloads_kb], rotation=90)
ax1.legend(loc='upper left')
plt.title('Vanilla vs Prototype (one_container): Mean')
plt.tight_layout()
mean_fig_path = 'vanilla_vs_proto_mean.png'
plt.savefig(mean_fig_path)
print(f"Mean figure saved to: {mean_fig_path}")
plt.show()

# Plot median only
fig, ax2 = plt.subplots(figsize=(10, 6))
bars1 = ax2.bar(x - bar_width/2, median_proto.values, bar_width, label='Prototype', color='tab:blue')
bars2 = ax2.bar(x + bar_width/2, median_vanilla.values, bar_width, label='Vanilla', color='tab:orange')

for bar in bars1:
    ax2.annotate(f'{bar.get_height():.1f}',
                 xy=(bar.get_x() + bar.get_width()/2, bar.get_height()),
                 xytext=(0, 3), textcoords='offset points',
                 ha='center', va='bottom', fontsize=8)
for bar in bars2:
    ax2.annotate(f'{bar.get_height():.1f}',
                 xy=(bar.get_x() + bar.get_width()/2, bar.get_height()),
                 xytext=(0, 3), textcoords='offset points',
                 ha='center', va='bottom', fontsize=8)


ax2.set_xlabel('Payload size (KB)')
ax2.set_ylabel('Median time (ms)')
ax2.set_xticks(x)
ax2.set_xticklabels([f"{p:.1f}" for p in payloads_kb], rotation=90)
ax2.legend(loc='upper left')
plt.title('Vanilla vs Prototype (one_container): Median')
plt.tight_layout()
median_fig_path = 'vanilla_vs_proto_median.png'
plt.savefig(median_fig_path)
print(f"Median figure saved to: {median_fig_path}")
plt.show()
