#!/usr/bin/env python3
"""
run_request_sweep.py -- Fresh-connection HTTPS sweep for the wolfSSL vs Caddy
request read/decrypt benchmark.

Each sample opens a brand-new TLS connection, optionally waits a short time after
the handshake so the server-side handshake is certainly outside the timed region,
sends exactly one HTTP/1.1 POST request with a fixed Content-Length body, then
reads the JSON timing response and writes one CSV row.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import socket
import ssl
import sys
import time
from typing import Any


DEFAULT_SIZES = [64, 256, 1024, 4096, 16384, 65536, 131072, 262144, 524288, 1048576]
DEFAULT_SAMPLES = 100
DEFAULT_TIMEOUT = 10.0
DEFAULT_PATH = "/bench"
DEFAULT_POST_HANDSHAKE_DELAY_MS = 5.0
DEFAULT_TLS_VERSION = "1.2"


def parse_sizes(value: str) -> list[int]:
    sizes: list[int] = []
    for chunk in value.split(","):
        item = chunk.strip()
        if not item:
            continue
        size = int(item)
        if size < 0:
            raise ValueError(f"payload size must be >= 0, got {size}")
        sizes.append(size)
    if not sizes:
        raise ValueError("at least one payload size is required")
    return sizes


def make_ssl_context(tls_version: str) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if tls_version == "1.2":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    elif tls_version == "1.3":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    else:
        raise ValueError(f"unsupported TLS version {tls_version}")
    ctx.set_alpn_protocols(["http/1.1"])
    return ctx


def recv_until_eof(sock: ssl.SSLSocket) -> bytes:
    chunks: list[bytes] = []
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


def parse_http_response(data: bytes) -> tuple[int, dict[str, str], bytes]:
    header_end = data.find(b"\r\n\r\n")
    if header_end < 0:
        raise ValueError("incomplete HTTP response headers")

    header_blob = data[:header_end].decode("iso-8859-1")
    body = data[header_end + 4 :]
    lines = header_blob.split("\r\n")
    if not lines or len(lines[0].split(" ")) < 2:
        raise ValueError("invalid HTTP status line")

    status_code = int(lines[0].split(" ")[1])
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()

    if "content-length" in headers:
        expected_len = int(headers["content-length"])
        if len(body) != expected_len:
            raise ValueError(
                f"response body length mismatch: expected {expected_len}, got {len(body)}"
            )

    return status_code, headers, body


def one_request(
    host: str,
    port: int,
    path: str,
    timeout: float,
    payload: bytes,
    post_handshake_delay_ms: float,
    tls_version: str,
) -> dict[str, Any]:
    ctx = make_ssl_context(tls_version)

    with socket.create_connection((host, port), timeout=timeout) as raw_sock:
        raw_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with ctx.wrap_socket(raw_sock, server_hostname=host) as tls_sock:
            tls_sock.settimeout(timeout)

            if post_handshake_delay_ms > 0:
                time.sleep(post_handshake_delay_ms / 1000.0)

            request = (
                f"POST {path} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                "Content-Type: application/octet-stream\r\n"
                f"Content-Length: {len(payload)}\r\n"
                "Connection: close\r\n"
                "\r\n"
            ).encode("ascii") + payload

            tls_sock.sendall(request)
            response = recv_until_eof(tls_sock)

    status_code, _, body = parse_http_response(response)
    if status_code != 200:
        raise RuntimeError(f"server returned HTTP {status_code}: {body.decode(errors='replace')}")

    result = json.loads(body.decode("utf-8"))
    if not isinstance(result, dict):
        raise RuntimeError("server did not return a JSON object")
    return result


def write_rows(path: str, rows: list[dict[str, Any]], append: bool) -> None:
    fieldnames = [
        "implementation",
        "payload_size",
        "sample_index",
        "server_request_no",
        "top1_rdtsc",
        "top2_rdtsc",
        "delta_cycles",
        "cntfrq",
        "delta_ns",
        "bytes_expected",
        "bytes_consumed",
        "tls_version",
        "cipher_suite",
        "host",
        "port",
        "path",
        "post_handshake_delay_ms",
        "requested_tls_version",
    ]

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    mode = "a" if append else "w"
    must_write_header = (not append) or (not os.path.exists(path)) or os.path.getsize(path) == 0

    with open(path, mode, newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        if must_write_header:
            writer.writeheader()
        writer.writerows(rows)


def run_sweep(args: argparse.Namespace) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    total = len(args.sizes) * args.samples
    done = 0

    for payload_size in args.sizes:
        payload = os.urandom(payload_size)

        for sample_index in range(1, args.samples + 1):
            done += 1
            print(
                f"[{done:4d}/{total}] implementation={args.implementation} "
                f"payload={payload_size:>8} sample={sample_index:>3}",
                flush=True,
            )

            result = one_request(
                host=args.host,
                port=args.port,
                path=args.path,
                timeout=args.timeout,
                payload=payload,
                post_handshake_delay_ms=args.post_handshake_delay_ms,
                tls_version=args.tls_version,
            )

            rows.append(
                {
                    "implementation": args.implementation,
                    "payload_size": payload_size,
                    "sample_index": sample_index,
                    "server_request_no": result.get("request_no", 0),
                    "top1_rdtsc": result.get("top1_rdtsc", 0),
                    "top2_rdtsc": result.get("top2_rdtsc", 0),
                    "delta_cycles": result.get("delta_cycles", 0),
                    "cntfrq": result.get("cntfrq", 1),
                    "delta_ns": result.get("delta_ns", 0),
                    "bytes_expected": result.get("bytes_expected", payload_size),
                    "bytes_consumed": result.get("bytes_consumed", 0),
                    "tls_version": result.get("tls_version", "unknown"),
                    "cipher_suite": result.get("cipher_suite", "unknown"),
                    "host": args.host,
                    "port": args.port,
                    "path": args.path,
                    "post_handshake_delay_ms": args.post_handshake_delay_ms,
                    "requested_tls_version": args.tls_version,
                }
            )

    return rows


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the wolfSSL vs Caddy request sweep")
    parser.add_argument("--implementation", required=True, choices=["wolfssl", "caddy"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--path", default=DEFAULT_PATH)
    parser.add_argument("--sizes", type=parse_sizes, default=DEFAULT_SIZES)
    parser.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--post-handshake-delay-ms", type=float, default=DEFAULT_POST_HANDSHAKE_DELAY_MS)
    parser.add_argument("--tls-version", choices=["1.2", "1.3"], default=DEFAULT_TLS_VERSION)
    parser.add_argument("--out", required=True)
    parser.add_argument("--append", action="store_true")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.samples <= 0:
        parser.error("--samples must be > 0")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.post_handshake_delay_ms < 0:
        parser.error("--post-handshake-delay-ms must be >= 0")

    rows = run_sweep(args)
    write_rows(args.out, rows, append=args.append)
    print(f"wrote {len(rows)} rows to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())