#!/bin/bash

# High-Concurrency System Tuning Script
# Run this with sudo to allow 15,000+ connections

echo "[*] Increasing somaxconn to 32768..."
sysctl -w net.core.somaxconn=32768

echo "[*] Increasing TCP SYN backlog to 32768..."
sysctl -w net.ipv4.tcp_max_syn_backlog=32768

echo "[*] Increasing IP local port range..."
sysctl -w net.ipv4.ip_local_port_range="1024 65535"

echo "[*] Enabling TCP fast recycling/reuse..."
sysctl -w net.ipv4.tcp_tw_reuse=1

echo "[*] Increasing File Descriptor limits (Session-only)..."
ulimit -n 65535

echo "[!] System Ready for High Concurrency."
