# Benchmark 3 Keepalive Runbook

This directory contains the bench3 keepalive workflow.

The prototype path is intentionally the same as bench2 keepalive, including the worker-side request-2+ ownership peek. The vanilla path is the only architectural change: there is no longer a standalone wolfSSL proxy process on the Pi. Instead, vanilla now uses the same custom faasd gateway image family as the prototype path, with an embedded wolfSSL listener on port `8444`.

## What is here

- `proto_worker/`: prototype OpenFaaS worker image source.
- `proto_gateway/`: shared gateway image source for both modes.
- `vanilla_function/`: plain function used behind the embedded vanilla TLS listener.
- `vanilla_proxy/`: source for the embedded vanilla listener binary, compiled into the gateway image.
- `client/run_combined_sweep.py`: combined payload sweep client, now with `--payload-sizes` support.
- `scripts/prepare_proto_stack.sh`: rebuild, redeploy, enable, and smoke-test the prototype stack.
- `scripts/prepare_vanilla_stack.sh`: rebuild, redeploy, enable, and smoke-test the vanilla stack.
- `scripts/run_single_alpha_combined.sh`: generic single-alpha runner.
- `scripts/run_alpha0_step32.sh`: bench3 helper for the requested `alpha=0`, `32 KiB`-only runs.

## Bench3 scope

- `alpha=0` only.
- `32 KiB` payload only.
- one-container evaluation.
- two-container evaluation.

In this benchmark, one-container means both logical slots resolve to the same deployed function name. Two-container means `function-a` and `function-b` remain distinct.

## Fairness rule

For cross-mode comparison, both prototype and vanilla must use the same `requests_per_conn` value. The default in the helper scripts is `50`.

The gateway and prototype-worker build scripts now default to the existing local buildx cache at `.buildx-cache/faasd-gateway`. That is the same cache family used for the previous wolfSSL/libtlspeek-heavy builds, so reruns should reuse the already compiled layers instead of rebuilding wolfSSL from zero.

## Alpha meaning

- `alpha=0`: strict alternation `A B A B ...`

For bench3, the one-container case keeps `alpha=0` but sets both logical slots to the same function name. That preserves the same client schedule shape while removing cross-container alternation.

## Prepare the prototype stack

From the repository root:

```bash
bash benchmarks/micro/micro-bench3-keepalive/scripts/prepare_proto_stack.sh
```

What it does:

1. Restores the base faasd gateway first if another custom benchmark gateway is active.
2. Builds and pushes the current prototype worker image from source, then redeploys `bench2-fn-a` and `bench2-fn-b` on the Pi.
3. Builds the shared bench3 gateway image, imports it on the Pi, and enables prototype mode on port `9444`.
4. Runs a smoke test against `https://192.168.2.2:9444/function/bench2-fn-a`.

Useful overrides:

```bash
PI_SSH=romero@192.168.2.2
PI_SUDO_PASSWORD='...'
BUILDER_NAME=bench2-arm64-builder
CACHE_DIR=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/.buildx-cache/faasd-gateway
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python
```

## Prepare the vanilla stack

From the repository root:

```bash
bash benchmarks/micro/micro-bench3-keepalive/scripts/prepare_vanilla_stack.sh
```

What it does:

1. Restores the base faasd gateway first if another custom benchmark gateway is active.
2. Builds and pushes the current vanilla function image from source, then redeploys `bench2-fn-a` and `bench2-fn-b` on the Pi.
3. Builds the same shared bench3 gateway image, imports it on the Pi, and enables vanilla mode on port `8444`.
4. Runs a smoke test against `https://192.168.2.2:8444/function/bench2-fn-a`.

The `vanilla_proxy/` directory is still present, but it is now build input for the gateway image. It is no longer copied to the Pi or launched as a standalone daemon.

## Requested evaluation commands

One container:

```bash
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_alpha0_step32.sh vanilla one
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_alpha0_step32.sh proto one
```

Two containers:

```bash
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_alpha0_step32.sh vanilla two
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_alpha0_step32.sh proto two
```

Defaults used by the helper:

- `alpha=0`
- `payload_sizes=32768`
- `requests_per_conn=50`
- `num_requests_per_payload=100`
- `warmup_per_payload=20`

You can override them with environment variables, for example:

```bash
REQUESTS_PER_CONN=25 \
NUM_REQUESTS_PER_PAYLOAD=200 \
WARMUP_PER_PAYLOAD=40 \
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_alpha0_step32.sh vanilla two
```

## Generic single-alpha runner

If you want to keep using the broader sweep client directly, `run_single_alpha_combined.sh` now passes `PAYLOAD_SIZES` through to `client/run_combined_sweep.py`.

Example:

```bash
PAYLOAD_SIZES=32768 \
NUM_REQUESTS_PER_PAYLOAD=100 \
WARMUP_PER_PAYLOAD=20 \
bash benchmarks/micro/micro-bench3-keepalive/scripts/run_single_alpha_combined.sh vanilla 0
```

## Quick validation commands

Prototype smoke test:

```bash
python3 benchmarks/micro/micro-bench3-keepalive/client/simple_test.py \
  --host 192.168.2.2 --port 9444 --fn bench2-fn-a --n 8 --payload 64
```

Vanilla smoke test:

```bash
python3 benchmarks/micro/micro-bench3-keepalive/client/simple_test.py \
  --host 192.168.2.2 --port 8444 --fn bench2-fn-a --n 8 --payload 64
```