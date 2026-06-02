import http.client
import json
import time

def main():
    host = "192.168.2.2"
    port = 8080
    
    print(f"Connecting to Gateway at {host}:{port}...")
    # Create a single TCP connection
    conn = http.client.HTTPConnection(host, port, timeout=10)
    
    # ---------------------------------------------------------
    # Request 1: timing-fn-a (Initial handoff)
    # ---------------------------------------------------------
    print("\n--> [1] Sending first request to /function/timing-fn-a (Keep-Alive)")
    start = time.time()
    payload_a = json.dumps({"numbers": [1, 2, 3]})
    conn.request("POST", "/function/timing-fn-a", body=payload_a, headers={"Connection": "keep-alive", "Content-Type": "application/json"})
    r1 = conn.getresponse()
    body1 = r1.read().decode()
    latency1 = (time.time() - start) * 1000
    
    print(f"    Status : {r1.status}")
    print(f"    Headers: {r1.getheaders()}")
    try:
        print(f"    Body   : {json.loads(body1)}")
    except:
        print(f"    Body   : {body1}")
    print(f"    Latency: {latency1:.2f} ms")
    
    time.sleep(1) # tiny pause

    # ---------------------------------------------------------
    # Request 2: timing-fn-b (Migration test)
    # ---------------------------------------------------------
    print("\n--> [2] Sending second request to /function/timing-fn-b on the SAME socket (Keep-Alive)")
    start = time.time()
    payload_b = json.dumps({"numbers": [4, 5, 6]})
    conn.request("POST", "/function/timing-fn-b", body=payload_b, headers={"Connection": "keep-alive", "Content-Type": "application/json"})
    r2 = conn.getresponse()
    body2 = r2.read().decode()
    latency2 = (time.time() - start) * 1000
    
    print(f"    Status : {r2.status}")
    print(f"    Headers: {r2.getheaders()}")
    try:
        print(f"    Body   : {json.loads(body2)}")
    except:
        print(f"    Body   : {body2}")
    print(f"    Latency: {latency2:.2f} ms")
    
    time.sleep(1)

    # ---------------------------------------------------------
    # Request 3: timing-fn-a (Migrate back)
    # ---------------------------------------------------------
    print("\n--> [3] Sending third request to /function/timing-fn-a on the SAME socket (Close)")
    start = time.time()
    payload_c = json.dumps({"numbers": [7, 8, 9]})
    conn.request("POST", "/function/timing-fn-a", body=payload_c, headers={"Connection": "close", "Content-Type": "application/json"})
    r3 = conn.getresponse()
    body3 = r3.read().decode()
    latency3 = (time.time() - start) * 1000
    
    print(f"    Status : {r3.status}")
    print(f"    Headers: {r3.getheaders()}")
    try:
        print(f"    Body   : {json.loads(body3)}")
    except:
        print(f"    Body   : {body3}")
    print(f"    Latency: {latency3:.2f} ms")

    conn.close()
    print("\nConnection closed successfully.")

if __name__ == "__main__":
    main()
