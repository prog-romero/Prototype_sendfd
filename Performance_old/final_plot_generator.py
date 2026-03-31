import subprocess
import time
import re
import matplotlib.pyplot as plt
import os

# --- SCIENTIFIC CONFIGURATION V6 (Real Nginx Baseline) ---
CONCURRENCY_LEVELS = [50, 100, 200, 500, 800, 1000]
WARMUP_DURATION = "3s"
TEST_DURATION = "15s"
THREADS = 4
DOMAIN = "sum.faas.local"

SERVER_CPUS = "0,1"
CLIENT_CPUS = "2,3"

ULIMIT_CMD = "ulimit -n 65535 && "

def run_cmd(cmd, pinned_cpus=None):
    try:
        if pinned_cpus:
            cmd = f"taskset -c {pinned_cpus} {cmd}"
        full_cmd = ULIMIT_CMD + cmd
        result = subprocess.run(full_cmd, shell=True, capture_output=True, text=True, timeout=200)
        return result.stdout
    except Exception as e:
        print(f"Erreur lors de l'exécution : {e}")
        return ""

def stop_all():
    print("🧹 Nettoyage des processus...")
    # Stop manual binaries
    subprocess.run("pkill -9 gateway; pkill -9 classic_proxy; pkill -9 worker_sum; pkill -9 worker_product; sleep 2", shell=True)
    # Stop Nginx if running
    subprocess.run("nginx -p . -c nginx_classic.conf -s stop > /dev/null 2>&1 || true", shell=True)
    subprocess.run("pkill -9 nginx; sleep 2", shell=True)

def start_direct():
    print(f"🚀 [DIRECT] Démarrage (CPU {SERVER_CPUS})...")
    subprocess.run(f"{ULIMIT_CMD} taskset -c {SERVER_CPUS} ./worker_sum > worker_direct.log 2>&1 &", shell=True)
    time.sleep(3)

def start_proto():
    print(f"🚀 [PROTOTYPE] Démarrage (CPU {SERVER_CPUS})...")
    subprocess.run(f"{ULIMIT_CMD} taskset -c {SERVER_CPUS} ./worker_sum > worker_proto.log 2>&1 &", shell=True)
    subprocess.run(f"{ULIMIT_CMD} taskset -c {SERVER_CPUS} ./gateway > gw_proto.log 2>&1 &", shell=True)
    time.sleep(3)

def start_classic_nginx():
    print(f"🚀 [CLASSIC NGINX] Démarrage (CPU {SERVER_CPUS})...")
    # Start worker for nginx to talk to (Nginx Proxy Mode)
    subprocess.run(f"{ULIMIT_CMD} taskset -c {SERVER_CPUS} ./worker_sum > worker_classic.log 2>&1 &", shell=True)
    # Start Nginx
    cmd = f"{ULIMIT_CMD} taskset -c {SERVER_CPUS} nginx -p . -c nginx_classic.conf > nginx_start.log 2>&1 &"
    subprocess.run(cmd, shell=True)
    time.sleep(5) # Give Nginx time to bind and Init

def parse_wrk_output(output):
    tput_mb = 0.0
    tput_match = re.search(r"Transfer/sec:\s+([\d.]+)(\w+)", output)
    if tput_match:
        val, unit = tput_match.groups()
        if unit.upper() == "KB": tput_mb = float(val) / 1024.0
        elif unit.upper() == "MB": tput_mb = float(val)
        elif unit.upper() == "GB": tput_mb = float(val) * 1024.0
        else: tput_mb = float(val) / (1024.0 * 1024.0)
    
    avg_us = 0.0
    latency_match = re.search(r"Latency\s+([\d.]+)(\w+)", output)
    if latency_match:
        val, unit = latency_match.groups()
        if unit == "ms": avg_us = float(val) * 1000.0
        elif unit == "s": avg_us = float(val) * 1000000.0
        else: avg_us = float(val)
        
    return tput_mb, avg_us

def run_bench_for_arch(arch_name, url):
    data = {"tput": [], "lat_avg": []}
    print(f"\n📈 Évaluation de {arch_name} (Client sur CPU {CLIENT_CPUS})...")
    for c in CONCURRENCY_LEVELS:
        print(f"  -> Concurrence {c}...")
        run_cmd(f"wrk -t{THREADS} -c{c} -d{WARMUP_DURATION} \"{url}\"", pinned_cpus=CLIENT_CPUS)
        time.sleep(1)
        out = run_cmd(f"wrk -t{THREADS} -c{c} -d{TEST_DURATION} \"{url}\"", pinned_cpus=CLIENT_CPUS)
        tput, l_avg = parse_wrk_output(out)
        data["tput"].append(tput)
        data["lat_avg"].append(l_avg)
        print(f"     [Résultat] {tput:.3f} MB/s | {l_avg:.1f} us")
        time.sleep(1)
    return data

def main():
    print(f"--- PROTOCOLE NGINX REAL-WORLD V6 ---")
    
    # 1. DIRECT
    stop_all()
    start_direct()
    data_direct = run_bench_for_arch("DIRECT", f"https://{DOMAIN}:8445/sum?a=10&b=20")
    
    # 2. PROTOTYPE (Ours)
    stop_all()
    start_proto()
    data_proto = run_bench_for_arch("PROTOTYPE", f"https://{DOMAIN}:8443/sum?a=10&b=20")
    
    # 3. CLASSIC (Real Nginx)
    stop_all()
    start_classic_nginx()
    data_base = run_bench_for_arch("CLASSIC (NGINX)", f"https://{DOMAIN}:8444/sum?a=10&b=20")
    
    stop_all()

    # --- PLOTS ---
    plt.figure(figsize=(14, 6))
    plt.subplot(1, 2, 1)
    plt.title("Évaluation : Latence (Baseline Nginx)")
    plt.plot(CONCURRENCY_LEVELS, data_direct["lat_avg"], 'o-', label="Direct", color='#2E7D32', alpha=0.5)
    plt.plot(CONCURRENCY_LEVELS, data_proto["lat_avg"], '^-', label="Prototype (sendfd)", color='#1565C0', linewidth=2)
    plt.plot(CONCURRENCY_LEVELS, data_base["lat_avg"], 's--', label="Classic NginX", color='#C62828')
    plt.xlabel("Connexions"); plt.ylabel("Latence (μs)"); plt.legend(); plt.grid(True, alpha=0.3)

    plt.subplot(1, 2, 2)
    plt.title("Évaluation : Débit (Baseline Nginx)")
    plt.plot(CONCURRENCY_LEVELS, data_direct["tput"], 'o-', label="Direct", color='#2E7D32', alpha=0.5)
    plt.plot(CONCURRENCY_LEVELS, data_proto["tput"], '^-', label="Prototype (sendfd)", color='#1565C0', linewidth=2)
    plt.plot(CONCURRENCY_LEVELS, data_base["tput"], 's--', label="Classic NginX", color='#C62828')
    plt.xlabel("Connexions"); plt.ylabel("MB/s"); plt.legend(); plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig("nginx_comparison.png")
    print("\n✅ Terminé. Graphique 'nginx_comparison.png' généré.")

if __name__ == "__main__":
    main()
