# Micro Benchmark: Forward TCP vs Migration

This benchmark reuses the keepalive benchmark structure, but changes the timing boundary so the compared costs are:

- vanilla: post-decrypt forward path from the copied TLS front-end toward the gateway/function path
- prototype: wrong-owner migration path on keepalive, where every measured request is relayed from the wrong worker to the right worker

## What is measured

### Vanilla

For each request on the persistent TLS connection:

1. the copied vanilla front-end decrypts the full HTTP request
2. it forwards the full plaintext request to the local gateway path
3. `top1` is stamped immediately after the full request write completes
4. the function stamps `top2` after it has fully read the request body
5. the copied front-end returns `delta = top2 - top1` to the client

This excludes the TLS decryption work from the timed window.

### Prototype

For each payload size, the client opens one fresh keepalive connection, sends one priming request to function A, then alternates targets `B, A, B, A, ...`.

That means every measured request arrives first on the wrong worker, which:

1. waits for readable bytes on the live TLS socket
2. stamps `top1`
3. peeks the request and detects the correct owner
4. exports TLS state and sends the fd back to the patched gateway
5. the patched gateway redispatches the same live connection to the right worker
6. the right worker reads the full request body and stamps `top2`

So every measured prototype request is a migration request.

## Payload sizes

The default sweep uses:

- 64 B
- 256 B
- 1 KiB
- 4 KiB
- 16 KiB
- 64 KiB
- 128 KiB
- 256 KiB
- 512 KiB
- 1 MiB

The request count per payload is weighted toward the smaller sizes by default, but you can force a uniform count for all sizes.

## Main files

- `client/run_combined_sweep.py`: main client for both modes
- `scripts/prepare_vanilla_stack.sh`: redeploy the copied vanilla path and smoke-test it
- `scripts/prepare_proto_stack.sh`: redeploy the copied prototype path and smoke-test it
- `scripts/run_single_alpha_combined.sh`: run one full payload sweep for `vanilla` or `proto`
- `vanilla_proxy/bench2_vanilla_proxy.c`: copied TLS front-end with the new vanilla timing boundary
- `vanilla_function/bench2_vanilla_fn.c`: function returning `top2` metadata to the copied front-end
- `proto_gateway/bench2gw/`: copied prototype gateway shim
- `proto_worker/bench2_proto_worker.c`: copied prototype worker used for wrong-owner migration timing

## Prepare the vanilla path

From the repository root:

```bash
bash benchmarks/micro/micro-bench-forward-tcp-vs-migration/scripts/prepare_vanilla_stack.sh
```

What it does:

1. restores the vanilla OpenFaaS gateway on the Pi
2. rebuilds and redeploys the copied vanilla function image
3. syncs and starts the copied vanilla TLS front-end on port `8444`
4. runs a smoke test against `https://<pi>:8444/function/bench2-fn-a`

## Run the vanilla sweep

```bash
python3 benchmarks/micro/micro-bench-forward-tcp-vs-migration/client/run_combined_sweep.py \
  --host 192.168.2.2 \
  --port 8444 \
  --label vanilla \
  --out benchmarks/micro/micro-bench-forward-tcp-vs-migration/results/combined_vanilla.csv
```

Uniform example:

```bash
python3 benchmarks/micro/micro-bench-forward-tcp-vs-migration/client/run_combined_sweep.py \
  --host 192.168.2.2 \
  --port 8444 \
  --label vanilla \
  --num-requests-per-payload 100 \
  --warmup-per-payload 20 \
  --out benchmarks/micro/micro-bench-forward-tcp-vs-migration/results/combined_vanilla_uniform.csv
```

## Prepare the prototype path

From the repository root:

```bash
bash benchmarks/micro/micro-bench-forward-tcp-vs-migration/scripts/prepare_proto_stack.sh
```

What it does:

1. rebuilds and redeploys the copied prototype worker image
2. rebuilds and enables the copied prototype gateway image
3. runs a smoke test against `https://<pi>:9444/function/bench2-fn-a`

## Run the prototype sweep

```bash
python3 benchmarks/micro/micro-bench-forward-tcp-vs-migration/client/run_combined_sweep.py \
  --host 192.168.2.2 \
  --port 9444 \
  --label proto \
  --out benchmarks/micro/micro-bench-forward-tcp-vs-migration/results/combined_proto.csv
```

Uniform example:

```bash
python3 benchmarks/micro/micro-bench-forward-tcp-vs-migration/client/run_combined_sweep.py \
  --host 192.168.2.2 \
  --port 9444 \
  --label proto \
  --num-requests-per-payload 100 \
  --warmup-per-payload 20 \
  --out benchmarks/micro/micro-bench-forward-tcp-vs-migration/results/combined_proto_uniform.csv
```

## Convenience wrapper

```bash
bash benchmarks/micro/micro-bench-forward-tcp-vs-migration/scripts/run_single_alpha_combined.sh vanilla
bash benchmarks/micro/micro-bench-forward-tcp-vs-migration/scripts/run_single_alpha_combined.sh proto
```

The filename is retained from the copied benchmark, but it now runs one fixed forward-vs-migration sweep rather than an alpha sweep.

## Output CSV columns

The client writes one CSV with these columns:

- `label`
- `alpha`
- `payload_size`
- `request_seq`
- `request_no`
- `worker`
- `target_fn`
- `switched`
- `delta_cycles`
- `cntfrq`
- `delta_ns`
- `requests_per_conn`
- `top1_rdtsc`
- `top2_rdtsc`

Notes:

- `alpha` is fixed to `0` for compatibility with the copied plotting code
- `requests_per_conn` is the effective per-run connection length: `1 prime + warmup + measured`
- only measured requests are written to the CSV

## Smoke test helper

```bash
python3 benchmarks/micro/micro-bench-forward-tcp-vs-migration/client/simple_test.py \
  --host 192.168.2.2 \
  --port 9444 \
  --fn bench2-fn-a \
  --n 8 \
  --payload 64
```

## Interpretation

This benchmark is meant to be combined conceptually with the separate TLS decryption benchmark:

- decryption benchmark: receive/decrypt cost only
- vanilla here: post-decrypt forward path
- prototype here: wrong-owner migration path only

It is therefore a decomposition benchmark, not a full end-to-end latency benchmark by itself.