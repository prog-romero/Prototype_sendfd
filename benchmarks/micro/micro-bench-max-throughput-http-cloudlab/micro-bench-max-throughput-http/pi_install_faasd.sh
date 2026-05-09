#!/usr/bin/env bash
set -euo pipefail

# Installs faasd + dependencies (containerd, CNI plugins, faas-cli)
# using the official upstream install script.
#
# Run on the Raspberry Pi:
#   bash pi_install_faasd.sh

curl -fsSL https://raw.githubusercontent.com/openfaas/faasd/master/hack/install.sh | bash
