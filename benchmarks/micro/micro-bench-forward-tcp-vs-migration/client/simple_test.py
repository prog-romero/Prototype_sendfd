#!/usr/bin/env python3
"""
simple_test.py — Send N requests to a single function and print latency.
Usage:
    python3 simple_test.py --host 192.168.2.2 --port 9444 --fn bench2-fn-a --n 50
"""
import argparse, http.client, json, os, ssl, time

def make_ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host",    default="192.168.2.2")
    ap.add_argument("--port",    type=int, default=9444)
    ap.add_argument("--fn",      default="bench2-fn-a")
    ap.add_argument("--n",       type=int, default=50)
    ap.add_argument("--payload", type=int, default=64, help="payload size in bytes")
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    payload = os.urandom(args.payload)
    conn = http.client.HTTPSConnection(args.host, port=args.port,
                                       context=make_ctx(), timeout=args.timeout)

    print(f"Sending {args.n} requests to https://{args.host}:{args.port}/function/{args.fn}")
    print(f"Payload: {args.payload} B   Timeout: {args.timeout}s\n")

    ok = 0
    for i in range(args.n):
        t0 = time.perf_counter()
        try:
            conn.request("POST", f"/function/{args.fn}", body=payload,
                         headers={"Content-Type": "application/octet-stream",
                                  "Content-Length": str(len(payload)),
                                  "Connection": "keep-alive"})
            resp = conn.getresponse()
            data = json.loads(resp.read().decode())
            elapsed_ms = (time.perf_counter() - t0) * 1000
            ok += 1
            print(f"  [{i+1:3d}] OK  {elapsed_ms:7.1f} ms  worker={data.get('worker','')}  "
                  f"delta_ns={data.get('delta_ns', data.get('delta_cycles','?'))}")
        except Exception as e:
            elapsed_ms = (time.perf_counter() - t0) * 1000
            print(f"  [{i+1:3d}] ERR {elapsed_ms:7.1f} ms  {e}")
            # reconnect
            try: conn.close()
            except: pass
            conn = http.client.HTTPSConnection(args.host, port=args.port,
                                               context=make_ctx(), timeout=args.timeout)

    conn.close()
    print(f"\n{ok}/{args.n} requests succeeded.")
    return 0 if ok == args.n else 1

if __name__ == "__main__":
    raise SystemExit(main())
