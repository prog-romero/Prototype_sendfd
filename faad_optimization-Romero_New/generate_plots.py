import matplotlib.pyplot as plt
import numpy as np

def generate_comparison_plots():
    # Données simulées basées sur les attentes théoriques (à remplacer par vos mesures réelles)
    scenarios = ['Baseline (Classic)', 'Prototype (Hot Potato)']
    
    # 1. Latence Moyenne (ms)
    latency = [12.5, 4.2] # ms
    
    # 2. Utilisation CPU Gateway (%)
    cpu_usage = [28.0, 3.5] # %
    
    # 3. Throughput (Req/s)
    throughput = [1200, 4500] # RPS

    fig, axs = plt.subplots(1, 3, figsize=(18, 5))

    # Graphe 1 : Latence (Histogramme)
    axs[0].bar(scenarios, latency, color=['red', 'green'])
    axs[0].set_title('Latence Moyenne (TTFB)')
    axs[0].set_ylabel('ms')

    # Graphe 2 : Charge CPU (Histogramme)
    axs[1].bar(scenarios, cpu_usage, color=['red', 'green'])
    axs[1].set_title('Charge CPU Gateway')
    axs[1].set_ylabel('% CPU')

    # Graphe 3 : Débit (Histogramme)
    axs[2].bar(scenarios, throughput, color=['red', 'green'])
    axs[2].set_title('Débit (Req/s)')
    axs[2].set_ylabel('RPS')

    plt.tight_layout()
    plt.savefig('performance_results.png')
    print("Graphique 'performance_results.png' généré avec succès.")

if __name__ == "__main__":
    generate_comparison_plots()
