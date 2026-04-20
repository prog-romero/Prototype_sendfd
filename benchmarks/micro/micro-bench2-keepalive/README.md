# Benchmark 2 Keepalive Runbook

This directory contains the repeatable workflow for the benchmark-2 keepalive comparison between the vanilla path and the prototype sendfd path.

## What is here

- `proto_worker/`: prototype OpenFaaS worker image source.
- `proto_gateway/`: prototype gateway shim source.
- `vanilla_function/`: plain function used behind the vanilla proxy.
- `vanilla_proxy/`: TLS proxy used for the vanilla measurements on port `8444`.
- `client/run_combined_sweep.py`: combined payload sweep writer for one or more alpha values.
- `client/plot_bench2_results.py`: fairness-oriented comparison plots.
- `scripts/prepare_proto_stack.sh`: rebuild, redeploy, enable, and smoke-test the prototype stack.
- `scripts/prepare_vanilla_stack.sh`: restore, redeploy, start, and smoke-test the vanilla stack.
- `scripts/run_single_alpha_combined.sh`: run one combined sweep for a single alpha and save it to one CSV.

## Important fairness rule

For cross-mode comparison, both prototype and vanilla must use the same `requests_per_conn` value. The recommended value for this benchmark is `50`, and the high-level helper scripts in this directory now default to that value.

## Alpha meaning used by the current client

- `alpha=0`: strict alternation `A B A B ...`
- `alpha=25`: 25 percent burst on `function-a`, 25 percent burst on `function-b`, 50 percent random
- `alpha=50`: two large consecutive bursts, no random tail
- `alpha=100`: special case, all requests are pinned to `function-a`

If you want `alpha=100` to target `bench2-fn-b` instead, swap the function arguments when running the client or the helper script so that `--function-a` is the function you want to pin.

## One-command prototype preparation

From the repository root:

```bash
bash benchmarks/micro/micro-bench2-keepalive/scripts/prepare_proto_stack.sh
```

What it does:

1. Builds and pushes the current prototype worker image from source and redeploys `bench2-fn-a` and `bench2-fn-b` on the Pi.
2. Builds and copies the current prototype gateway image from source, imports it on the Pi, and enables it.
3. Runs a smoke test against `https://192.168.2.2:9444/function/bench2-fn-a`.

Useful environment overrides:

```bash
PI_SSH=romero@192.168.2.2
PI_SUDO_PASSWORD='...'
BUILDER_NAME=bench2-arm64-builder
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python
```

Note: OpenFaaS function deploys always pull from a registry, so the prototype worker deploy script intentionally pushes to `ttl.sh` instead of relying on a local-only image.

## Prototype alpha=0 and alpha=100 captures

For the dedicated prototype runs you asked for, use a uniform payload schedule:
10 payload sizes with 100 measured requests each, for 1000 measured rows per file.

```bash
NUM_REQUESTS_PER_PAYLOAD=100 WARMUP_PER_PAYLOAD=20 \
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 0

NUM_REQUESTS_PER_PAYLOAD=100 WARMUP_PER_PAYLOAD=20 \
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 100
```

The helper passes `--num-requests-per-payload` and `--warmup-per-payload` through to `client/run_combined_sweep.py`, so every payload size gets the same measurement weight.

Files captured on 2026-04-17:

- `results/proto_alpha0_uniform1000_rpc50_20260417_145546.csv`
- `results/proto_alpha100_uniform1000_rpc50_20260417_145656.csv`

If you want to pin `alpha=100` to `bench2-fn-b` instead of `bench2-fn-a`:

```bash
NUM_REQUESTS_PER_PAYLOAD=100 WARMUP_PER_PAYLOAD=20 \
FUNCTION_A=bench2-fn-b FUNCTION_B=bench2-fn-a \
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 100
```

## One-command vanilla preparation

Before any vanilla benchmark run, bring the vanilla path back up explicitly:

```bash
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/prepare_vanilla_stack.sh
```

What it does:

1. Restores the faasd vanilla gateway on the Pi if the prototype gateway is currently enabled.
2. Builds and pushes the current vanilla function image from source and redeploys `bench2-fn-a` and `bench2-fn-b`.
3. Syncs the vanilla proxy source to the Pi, rebuilds it there, starts it on port `8444`, and retries the smoke test until the proxy is ready.

Useful environment overrides:

```bash
PI_SSH=romero@192.168.2.2
PI_HOST=192.168.2.2
PI_REPO_ROOT=~/Prototype_sendfd
PI_SUDO_PASSWORD='...'
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python
```

## Fair vanilla rerun with the same RPC value

For the full fair combined sweep with `requests_per_conn=50`:

```bash
/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
  benchmarks/micro/micro-bench2-keepalive/client/run_combined_sweep.py \
  --host 192.168.2.2 \
  --port 8444 \
  --label vanilla \
  --requests-per-conn 50 \
  --out benchmarks/micro/micro-bench2-keepalive/results/combined_vanilla_rpc50.csv
```

For matched one-alpha vanilla files later:

```bash
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/prepare_vanilla_stack.sh

NUM_REQUESTS_PER_PAYLOAD=100 WARMUP_PER_PAYLOAD=20 \
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh vanilla 0

NUM_REQUESTS_PER_PAYLOAD=100 WARMUP_PER_PAYLOAD=20 \
PYTHON_BIN=/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh vanilla 100
```

Use the same `NUM_REQUESTS_PER_PAYLOAD`, `WARMUP_PER_PAYLOAD`, and `REQUESTS_PER_CONN` values as the prototype files when you generate the matching vanilla files.

## Regenerating the plots

After you have the new fair files:

```bash
/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
  benchmarks/micro/micro-bench2-keepalive/client/plot_bench2_results.py \
  --vanilla benchmarks/micro/micro-bench2-keepalive/results/combined_vanilla_rpc50.csv \
  --proto benchmarks/micro/micro-bench2-keepalive/results/combined_proto_quiet_20260417_132809.csv
```

Or point `--proto` and `--vanilla` to the alpha-specific CSV files when you want a one-alpha comparison.

## Quick validation commands

Prototype smoke test:

```bash
/home/tchiaze/CIAC_Triance/api-ml/venv/bin/python \
  benchmarks/micro/micro-bench2-keepalive/client/simple_test.py \
  --host 192.168.2.2 --port 9444 --fn bench2-fn-a --n 8 --payload 64
```

Vanilla helper scripts and prototype helper scripts in `scripts/` now pass `--requests-per-conn 50` explicitly so that they do not silently drift back to mismatched connection reuse.