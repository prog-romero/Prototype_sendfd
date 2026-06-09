import http.client
import json

conn = http.client.HTTPConnection("127.0.0.1", 8080, timeout=10)

# 1. Request to timing-fn-a
print("Sending first request to timing-fn-a...")
conn.request("POST", "/function/timing-fn-a", body="Payload-A", headers={"Connection": "keep-alive"})
r1 = conn.getresponse()
print("Response 1:", r1.status, r1.read().decode())

print("\nSending second request to timing-fn-b on the same connection...")
# 2. Request to timing-fn-b
conn.request("POST", "/function/timing-fn-b", body="Payload-B", headers={"Connection": "close"})
r2 = conn.getresponse()
print("Response 2:", r2.status, r2.read().decode())

conn.close()
