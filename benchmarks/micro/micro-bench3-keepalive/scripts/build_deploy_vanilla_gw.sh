#!/usr/bin/env bash
# build_deploy_vanilla_gw.sh
#
# Reuses the shared bench3 keepalive gateway image build/import flow, but
# enables the Pi-side vanilla keepalive mode on port 8444.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ENABLE_HELPER="pi_enable_vanilla_gw.sh" exec bash "${SCRIPT_DIR}/build_deploy_proto_gw.sh"