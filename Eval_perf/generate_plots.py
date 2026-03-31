import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os

def generate_plots():
    try:
        df = pd.read_csv('consolidated_results.csv')
    except Exception as e:
        print(f"Error reading consolidated_results.csv: {e}")
        return

    # Convert numeric columns to float
    numeric_cols = ['Connections', 'RPS', 'Throughput_MBs', 'AvgLatency_ms', 'P99Latency_ms', 
                    'ConnectErrors', 'ReadErrors', 'WriteErrors', 'Timeouts', 'AvgCPU_pct']
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce').fillna(0.0)
        else:
            df[col] = 0.0

    # Calculate total errors
    df['TotalErrors'] = df['ConnectErrors'] + df['ReadErrors'] + df['WriteErrors'] + df['Timeouts']

    # Set aesthetics
    sns.set_theme(style="whitegrid")
    palette = sns.color_palette("husl", 3)

    # 1. Throughput (RPS)
    plt.figure(figsize=(10, 6))
    sns.lineplot(data=df, x='Connections', y='RPS', hue='Mode', marker='o', palette=palette)
    plt.title('Throughput comparison: Requests Per Second', fontsize=15)
    plt.ylabel('RPS')
    plt.xlabel('Concurrency (Connections)')
    plt.savefig('throughput_rps.png', dpi=300)
    plt.close()

    # 2. Throughput (MB/s)
    plt.figure(figsize=(10, 6))
    sns.lineplot(data=df, x='Connections', y='Throughput_MBs', hue='Mode', marker='o', palette=palette)
    plt.title('Throughput comparison: MB/s', fontsize=15)
    plt.ylabel('MB/s')
    plt.xlabel('Concurrency (Connections)')
    plt.savefig('throughput_mbs.png', dpi=300)
    plt.close()

    # 3. Latency (Avg)
    plt.figure(figsize=(10, 6))
    sns.lineplot(data=df, x='Connections', y='AvgLatency_ms', hue='Mode', marker='o', palette=palette)
    plt.title('Average Latency comparison (ms)', fontsize=15)
    plt.ylabel('Latency (ms)')
    plt.xlabel('Concurrency (Connections)')
    plt.savefig('latency.png', dpi=300)
    plt.close()

    # 4. Errors
    plt.figure(figsize=(10, 6))
    sns.lineplot(data=df, x='Connections', y='TotalErrors', hue='Mode', marker='x', palette=palette)
    plt.title('Total Socket Errors & Timeouts', fontsize=15)
    plt.ylabel('Error Count')
    plt.xlabel('Concurrency (Connections)')
    plt.savefig('errors.png', dpi=300)
    plt.close()

    # 5. CPU Usage
    plt.figure(figsize=(10, 6))
    cpu_df = df.groupby('Mode')['AvgCPU_pct'].mean().reset_index()
    sns.barplot(data=cpu_df, x='Mode', y='AvgCPU_pct', hue='Mode', palette=palette, legend=False)
    plt.title('Average Server CPU Consumption (%)', fontsize=15)
    plt.ylabel('CPU %')
    plt.savefig('cpu_usage.png', dpi=300)
    plt.close()

if __name__ == "__main__":
    generate_plots()
