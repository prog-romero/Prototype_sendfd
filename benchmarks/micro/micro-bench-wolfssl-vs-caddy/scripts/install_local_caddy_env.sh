#!/usr/bin/env bash
set -euo pipefail

GO_MIN_VERSION="1.25.0"
GO_VERSION="${GO_VERSION:-1.25.3}"
GO_ROOT="${GO_ROOT:-/usr/local/go}"

if ! command -v apt-get >/dev/null 2>&1; then
    echo "ERROR: this helper currently supports Debian/Ubuntu systems with apt-get." >&2
    exit 1
fi

if [[ ! -r /etc/os-release ]]; then
    echo "ERROR: cannot read /etc/os-release." >&2
    exit 1
fi

. /etc/os-release
if [[ "${ID:-}" != "ubuntu" && "${ID:-}" != "debian" ]]; then
    echo "WARNING: this script was written for Debian/Ubuntu. Continuing anyway." >&2
fi

case "$(uname -m)" in
    x86_64|amd64)
        GO_ARCH="amd64"
        ;;
    aarch64|arm64)
        GO_ARCH="arm64"
        ;;
    *)
        echo "ERROR: unsupported architecture $(uname -m) for Go tarball install." >&2
        exit 1
        ;;
esac

have_modern_go() {
    if ! command -v go >/dev/null 2>&1; then
        return 1
    fi

    local current
    current="$(go version | awk '{print $3}' | sed 's/^go//')"
    if [[ -z "$current" ]]; then
        return 1
    fi

    if [[ "$(printf '%s\n%s\n' "$GO_MIN_VERSION" "$current" | sort -V | head -n1)" == "$GO_MIN_VERSION" ]]; then
        return 0
    fi

    return 1
}

echo "[1/4] Installing Caddy package prerequisites"
sudo apt-get update
sudo apt-get install -y \
    apt-transport-https \
    build-essential \
    ca-certificates \
    curl \
    debian-archive-keyring \
    debian-keyring \
    git \
    gpg

echo "[2/4] Installing official Caddy package repository"
if [[ ! -f /usr/share/keyrings/caddy-stable-archive-keyring.gpg ]]; then
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
        | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
fi

if [[ ! -f /etc/apt/sources.list.d/caddy-stable.list ]]; then
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
        | sudo tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
fi

sudo apt-get update
sudo apt-get install -y caddy

echo "[3/4] Ensuring Go ${GO_MIN_VERSION}+ is available for source builds"
if have_modern_go; then
    echo "Go is already new enough: $(go version)"
else
    tmp_tar="$(mktemp /tmp/go-toolchain.XXXXXX.tar.gz)"
    trap 'rm -f "$tmp_tar"' EXIT

    echo "Installing Go ${GO_VERSION} for linux-${GO_ARCH} under ${GO_ROOT}"
    curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" -o "$tmp_tar"
    sudo rm -rf "$GO_ROOT"
    sudo tar -C /usr/local -xzf "$tmp_tar"

    if [[ ":$PATH:" != *":/usr/local/go/bin:"* ]]; then
        echo
        echo "Add this to your shell profile before building instrumented Caddy:"
        echo "  export PATH=/usr/local/go/bin:\$PATH"
        echo
    fi
fi

echo "[4/4] Verifying installation"
caddy version
if command -v /usr/local/go/bin/go >/dev/null 2>&1; then
    /usr/local/go/bin/go version
elif command -v go >/dev/null 2>&1; then
    go version
fi

echo
echo "Next recommended checks:"
echo "  sudo systemctl status caddy --no-pager"
echo "  sudo journalctl -u caddy -n 50 --no-pager"
echo "  sudo caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile"