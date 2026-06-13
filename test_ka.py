import http.client
import json
import ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

conn = http.client.HTTPSConnection("192.168.2.2", 8443, timeout=10, context=ctx)

print("--> Envoi de la PREMIÈRE requête vers /function/vanilla-fn-a")
conn.request("POST", "/function/vanilla-fn-a", body="32 512", headers={"Connection": "keep-alive"})
r1 = conn.getresponse()
print("Response 1:", r1.status, json.loads(r1.read().decode()))

print("\n--> Envoi de la SECONDE requête vers /function/vanilla-fn-b sur la MÊME SOCKET TLS")
conn.request("POST", "/function/vanilla-fn-b", body="32 512", headers={"Connection": "close"})
r2 = conn.getresponse()
print("Response 2:", r2.status, json.loads(r2.read().decode()))

conn.close()
