import matplotlib.pyplot as plt
import numpy as np

def generate_article_plots():
    # Simulation de données basées sur les tendances observées 
    # (Ces données seront à remplacer par les vrais résultats du benchmark.py)
    concurrency = [50, 100, 200, 500]
    
    # 1. Throughput (req/s)
    throughput_base = [2500, 3100, 3400, 3500] # Saturation CPU plus tôt
    throughput_proto = [2800, 3800, 4500, 5200] # Plus de marge
    
    # 2. Gateway CPU Load (%)
    cpu_base = [15, 35, 65, 95] # Croissance linéaire
    cpu_proto = [2, 3, 4, 5]     # Quasi constant (juste PEEK)
    
    # 3. Latency p50 (ms)
    latency_base = [12, 18, 35, 60]
    latency_proto = [3, 4, 6, 12]

    # Plot Setup
    plt.style.use('bmh')
    fig, axs = plt.subplots(1, 3, figsize=(18, 5))
    
    # Panel A: Throughput
    axs[0].plot(concurrency, throughput_proto, 'o-', label='Prototype (sendfd)', color='blue')
    axs[0].plot(concurrency, throughput_base, 's--', label='Baseline (Nginx-style)', color='red')
    axs[0].set_title('Max Throughput (Fixed CPU Budget)')
    axs[0].set_xlabel('Concurrent Connections')
    axs[0].set_ylabel('Requests per Second')
    axs[0].legend()
    
    # Panel B: Gateway CPU
    axs[1].plot(concurrency, cpu_proto, 'o-', label='Prototype (sendfd)', color='blue')
    axs[1].plot(concurrency, cpu_base, 's--', label='Baseline (Nginx-style)', color='red')
    axs[1].set_title('Gateway CPU Load')
    axs[1].set_xlabel('Concurrent Connections')
    axs[1].set_ylabel('CPU Usage (%)')
    axs[1].legend()
    
    # Panel C: Latency
    axs[2].plot(concurrency, latency_proto, 'o-', label='Prototype (sendfd)', color='blue')
    axs[2].plot(concurrency, latency_base, 's--', label='Baseline (Nginx-style)', color='red')
    axs[2].set_title('Median Latency (p50)')
    axs[2].set_xlabel('Concurrent Connections')
    axs[2].set_ylabel('Latency (ms)')
    axs[2].legend()
    
    plt.tight_layout()
    plt.savefig('performance_evaluation.png')
    print("Graphique 'performance_evaluation.png' généré avec succès.")

if __name__ == "__main__":
    generate_article_plots()
