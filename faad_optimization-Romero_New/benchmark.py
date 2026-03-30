import socket
import ssl
import time
import argparse
import statistics

def run_benchmark(host, port, endpoint, num_requests, keep_alive):
    context = ssl._create_unverified_context()
    latencies = []
    
    start_time = time.time()
    
    if keep_alive:
        with socket.create_connection((host, port)) as sock:
            with context.wrap_socket(sock, server_hostname=host) as ssock:
                for i in range(num_requests):
                    req_start = time.time()
                    request = f"GET {endpoint} HTTP/1.1\r\nHost: {host}\r\n\r\n"
                    ssock.sendall(request.encode())
                    response = ssock.recv(4096)
                    latencies.append(time.time() - req_start)
    else:
        for i in range(num_requests):
            req_start = time.time()
            with socket.create_connection((host, port)) as sock:
                with context.wrap_socket(sock, server_hostname=host) as ssock:
                    request = f"GET {endpoint} HTTP/1.1\r\nHost: {host}\r\n\r\n"
                    ssock.sendall(request.encode())
                    response = ssock.recv(4096)
                    latencies.append(time.time() - req_start)
    
    total_duration = time.time() - start_time
    rps = num_requests / total_duration
    
    print(f"\n--- Benchmark Results for {endpoint} ---")
    print(f"Total Requests: {num_requests}")
    print(f"Keep-Alive: {keep_alive}")
    print(f"Total Duration: {total_duration:.4f}s")
    print(f"Requests per second: {rps:.2f}")
    if latencies:
        print(f"Avg Latency: {statistics.mean(latencies)*1000:.2f}ms")
        print(f"Min Latency: {min(latencies)*1000:.2f}ms")
        print(f"Max Latency: {max(latencies)*1000:.2f}ms")
    print("------------------------------------------")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FaaS Benchmarking Tool")
    parser.add_argument("--host", default="localhost", help="Target host")
    parser.add_argument("--port", type=int, default=8443, help="Target port")
    parser.add_argument("--endpoint", default="/sum?a=10&b=20", help="Target endpoint")
    parser.add_argument("-n", "--num-requests", type=int, default=50, help="Number of requests")
    parser.add_argument("--no-keep-alive", action="store_true", help="Disable keep-alive")
    
    args = parser.parse_args()
    run_benchmark(args.host, args.port, args.endpoint, args.num_requests, not args.no_keep_alive)
