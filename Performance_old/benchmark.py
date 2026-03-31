import socket
import ssl
import time
import argparse
import statistics
import subprocess
import os

def run_benchmark(host, port, endpoint, num_requests, keep_alive):
    context = ssl._create_unverified_context()
    latencies = []
    
    # SNI Hostname for port 8443
    hostname = "sum.faas.local"
    
    start_time = time.time()
    
    success = 0
    for i in range(num_requests):
        req_start = time.time()
        try:
            with socket.create_connection((host, port), timeout=5) as sock:
                # Add SNI for our Gateway
                with context.wrap_socket(sock, server_hostname=hostname) as ssock:
                    request = f"GET {endpoint} HTTP/1.1\r\nHost: {hostname}\r\nConnection: {'keep-alive' if keep_alive else 'close'}\r\n\r\n"
                    ssock.sendall(request.encode())
                    response = ssock.recv(4096)
                    if response:
                        success += 1
                        latencies.append(time.time() - req_start)
        except Exception as e:
            print(f"Error: {e}")
    
    total_duration = time.time() - start_time
    rps = success / total_duration if total_duration > 0 else 0
    
    return {
        "rps": rps,
        "avg_latency": statistics.mean(latencies)*1000 if latencies else 0,
        "p50_latency": statistics.median(latencies)*1000 if latencies else 0,
        "success_rate": (success/num_requests)*100
    }

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FaaS Benchmarking for Article")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("-n", "--num-requests", type=int, default=100)
    
    args = parser.parse_args()
    
    print(f"Running benchmark on Prototype (Port 8443)...")
    proto_res = run_benchmark(args.host, 8443, "/sum?a=10&b=20", args.num_requests, True)
    
    print(f"Running benchmark on Baseline (Port 8444)...")
    base_res = run_benchmark(args.host, 8444, "/sum?a=10&b=20", args.num_requests, True)
    
    print("\n" + "="*40)
    print(f"{'Metric':<20} | {'Prototype':<10} | {'Baseline':<10}")
    print("-" * 40)
    print(f"{'RPS':<20} | {proto_res['rps']:>10.2f} | {base_res['rps']:>10.2f}")
    print(f"{'Latency p50 (ms)':<20} | {proto_res['p50_latency']:>10.2f} | {base_res['p50_latency']:>10.2f}")
    print(f"{'Success Rate (%)':<20} | {proto_res['success_rate']:>10.1f} | {base_res['success_rate']:>10.1f}")
    print("="*40)
