#!/bin/bash

# CLEANUP EVERYTHING - Start fresh (NO SUDO)

echo "Cleaning up all evaluation artifacts..."

# Kill all potentially running processes (no sudo needed)
echo "[*] Killing any running servers..."
pkill -f "gateway" 2>/dev/null || true
pkill -f "worker" 2>/dev/null || true
pkill -f "proxy_worker" 2>/dev/null || true
pkill -f "nginx" 2>/dev/null || true
pkill -f "direct_tls_server" 2>/dev/null || true
sleep 1

# Remove socket files
echo "[*] Removing socket files..."
rm -f *.sock 2>/dev/null || true
rm -f /tmp/nginx.pid 2>/dev/null || true

# Remove log files
echo "[*] Removing log files..."
rm -f *.log 2>/dev/null || true

# Remove benchmark outputs
echo "[*] Removing benchmark outputs..."
rm -f *.txt 2>/dev/null || true
rm -f *.csv 2>/dev/null || true

# Remove generated configs
echo "[*] Removing generated configs..."
rm -f nginx_upstream.conf 2>/dev/null || true
rm -f nginx_active.conf 2>/dev/null || true

echo "[✓] Cleanup complete!"
