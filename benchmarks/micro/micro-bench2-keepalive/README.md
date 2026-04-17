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

After the prototype stack is ready, run the two single-alpha files you asked for:

```bash
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 0
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 100
```

These commands save one CSV per alpha under `benchmarks/micro/micro-bench2-keepalive/results/` with a timestamp in the file name.

If you want to pin `alpha=100` to `bench2-fn-b` instead of `bench2-fn-a`:

```bash
FUNCTION_A=bench2-fn-b FUNCTION_B=bench2-fn-a \
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh proto 100
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
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh vanilla 0
bash benchmarks/micro/micro-bench2-keepalive/scripts/run_single_alpha_combined.sh vanilla 100
```

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