#!/usr/bin/env bash
set -euo pipefail

echo "[Pi] faasd: $(command -v faasd || true)"
faasd version || true

echo "[Pi] systemd units:"
sudo systemctl status containerd --no-pager --full 2>/dev/null | head -40 || true
echo "---"
sudo systemctl status faasd-provider --no-pager --full 2>/dev/null | head -60 || true
echo "---"
sudo systemctl status faasd --no-pager --full 2>/dev/null | head -60 || true

echo "[Pi] listening ports (8080):"
ss -tlnp 2>/dev/null | grep -E ":8080\b" || true

echo "[Pi] HTTP probe (expect 301/302/401 depending on gateway build):"
if command -v curl >/dev/null 2>&1; then
  curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/ || true
else
  echo "curl missing"
fi
