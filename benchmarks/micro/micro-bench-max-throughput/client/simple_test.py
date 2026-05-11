#!/usr/bin/env python3
"""Small HTTPS smoke test for the max-throughput benchmark gateways."""

from __future__ import annotations

import argparse
import http.client
import ssl
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.2.2")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--fn", default="bench2-fn-a")
    parser.add_argument("--fn-b", default="bench2-fn-b")
    parser.add_argument("--alternate", action="store_true")
    parser.add_argument("--n", type=int, default=4)
    parser.add_argument("--payload", type=int, default=64)
    args = parser.parse_args()

    ctx = ssl._create_unverified_context()
    body = b"x" * args.payload

    conn = http.client.HTTPSConnection(
        args.host,
        args.port,
        context=ctx,
        timeout=10,
    )
    try:
        for i in range(args.n):
            fn = args.fn_b if args.alternate and i % 2 else args.fn
            path = f"/function/{fn}?a=10&b=20"
            conn.request(
                "POST",
                path,
                body=body,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Content-Length": str(len(body)),
                    "Connection": "keep-alive" if i + 1 < args.n else "close",
                },
            )
            resp = conn.getresponse()
            data = resp.read()
            if resp.status != 200:
                print(f"[error] request {i + 1}: HTTP {resp.status} body={data[:160]!r}")
                return 1
            print(f"[ok] request {i + 1}/{args.n}: HTTP {resp.status} {data[:160].decode('utf-8', 'replace')}")
    finally:
        conn.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
