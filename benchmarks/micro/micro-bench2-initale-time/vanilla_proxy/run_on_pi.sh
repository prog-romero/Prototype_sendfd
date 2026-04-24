#!/usr/bin/env bash
set -euo pipefail

echo "benchmarks/micro/micro-bench2-initale-time no longer uses the forwarded vanilla proxy." >&2
echo "Use the direct-HTTPS faasd gateway workflow instead:" >&2
echo "  bash benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_vanilla_gateway.sh" >&2
exit 1
