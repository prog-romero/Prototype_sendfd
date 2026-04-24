# micro-bench2-initale-time — Scenario A with rdtsc top1/top2

This benchmark is the bench-2 rdtsc version of the old initial-time scenario.

It keeps the same high-level goal as `micro-bench1-initale-time`, but changes the timing contract and the vanilla path:

- timing is now based on `top1_rdtsc`, `top2_rdtsc`, `delta_cycles`, `cntfrq`, and `delta_ns`
- vanilla no longer uses the forwarded wolfSSL proxy
- vanilla now uses a patched faasd gateway that accepts HTTPS directly on `:8443`
- prototype keeps the tlsmigrate/sendfd path on `:9443`
- prototype gateway peeks only enough request-line/header prefix to identify the function name before migration
- top2 is still stamped only when the downstream function or worker has fully consumed the request it receives

Vanilla and prototype are still meant to be run one after the other, not at the same time.

## 1. What is measured

Condition:
- one new TLS connection per request
- `Connection: close`
- same Raspberry Pi machine for gateway/provider/function, so the architectural counter is shared

Measured fields:
- `top1_rdtsc`: benchmark start stamp
- `top2_rdtsc`: benchmark stop stamp
- `delta_cycles`: `top2_rdtsc - top1_rdtsc`
- `cntfrq`: architectural counter frequency used to convert cycles to time
- `delta_ns`: nanosecond conversion of `delta_cycles`
- `client_rtt_ns`: end-to-end client-side request time on the laptop/workstation

Start and stop points:
- vanilla:
  - `top1`: request bytes are already in the gateway kernel receive buffer and the direct-HTTPS faasd gateway is just about to do the first real consuming read
  - `top2`: the vanilla function container has fully consumed the HTTP request body
- prototype:
  - `top1`: request bytes are already in the gateway kernel receive buffer and the patched gateway is just about to do `tls_read_peek()`
  - gateway stops the logical peek once it has enough plaintext to identify the function name for routing
  - `top2`: the prototype worker has restored TLS state and fully consumed the HTTP request body with real `wolfSSL_read()`

## 2. Pipelines under test

### Vanilla

Path:

`client (HTTPS)` -> `patched faasd gateway :8443` -> `timing-fn (HTTP, C container)`

Key points:
- no external forwarding proxy in front of the gateway
- the faasd gateway itself accepts HTTPS directly
- the gateway stamps top1 and injects it into the forwarded request headers
- the function container stamps top2 after the full body read

### Prototype

Path:

`client (HTTPS)` -> `patched faasd gateway :9443` -> `tls_read_peek()` -> `serialize TLS state + sendfd(SCM_RIGHTS)` -> `timing-fn worker (C container, direct TLS I/O)`

Key points:
- patched `faasd` provider mounts `/run/tlsmigrate` into function containers
- patched gateway uses `wolfSSL + libtlspeek`
- gateway stamps top1 immediately before the non-consuming peek that grows only until the function name can be identified
- worker restores the session and stamps top2 after full body consumption

## 3. Files involved

Main benchmark files:
- `benchmarks/micro/micro-bench2-initale-time/client/run_payload_sweep.py`
- `benchmarks/micro/micro-bench2-initale-time/client/plot_vanilla_vs_proto.py`
- `benchmarks/micro/micro-bench2-initale-time/function/timing_fn/timing_fn.c`
- `benchmarks/micro/micro-bench2-initale-time/proto_function/timing-fn/timing_fn_worker.c`
- `benchmarks/micro/micro-bench2-initale-time/common/bench2_rdtsc.h`

Gateway and mode-switch helpers:
- `benchmarks/micro/micro-bench2-initale-time/proto_gateway/`
- `benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_vanilla_gateway.sh`
- `benchmarks/micro/micro-bench2-initale-time/scripts/pi_enable_vanilla_gateway.sh`
- `benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_proto_gateway.sh`
- `benchmarks/micro/micro-bench2-initale-time/scripts/pi_enable_proto_gateway.sh`
- `benchmarks/micro/micro-bench2-initale-time/scripts/pi_restore_vanilla_gateway.sh`

Provider support reused from the existing prototype tree:
- `prototype/faasd_tlsmigrate/provider/build_faasd_arm64.sh`
- `prototype/faasd_tlsmigrate/provider/pi_install_proto_provider.sh`
- `prototype/faasd_tlsmigrate/provider/pi_restore_vanilla_provider.sh`

## 4. One-time prerequisites

On the Pi:
- `faasd` installed and working
- `faas-cli` installed and logged in at least once
- repository cloned at `~/Prototype_sendfd`
- SSH access working with `ssh romero@192.168.2.2`

Quick health check on the Pi:

```bash
curl -sS http://127.0.0.1:8080/healthz
```

Optional benchmark-local health check from the laptop/workstation:

```bash
export PI_SSH=romero@192.168.2.2

ssh -t ${PI_SSH} \
  'cd ~/Prototype_sendfd/benchmarks/micro/micro-bench2-initale-time && bash pi_verify_faasd.sh'
```

## 5. Sync benchmark files to the Pi

Run from the laptop/workstation:

```bash
export PI_SSH=romero@192.168.2.2

cd ~/Prototype_sendfd

bash benchmarks/micro/micro-bench2-initale-time/scripts/sync_code_to_pi.sh
```

This mirrors only the two paths that bench2 actually needs on the Pi:
- `benchmarks/micro/micro-bench2-initale-time/`
- `prototype/faasd_tlsmigrate/provider/`

The helper uses `rsync --delete`, then compares file checksums on both sides. If it finishes with `[ok]`, the synced benchmark code is identical on the laptop and on the Pi for those paths.

## 5b. Avoid recompiling wolfSSL for every prototype build

Important distinction:
- vanilla and prototype do not use the same runtime path
- vanilla uses the direct-HTTPS wolfSSL path enabled with `BENCH2_TLS_ENABLE`
- prototype uses the tlsmigrate + `tls_read_peek()` + sendfd path enabled with `TLSMIGRATE_ENABLE`

What is shared is the built gateway image artifact: the benchmark-local gateway Dockerfile compiles one custom OpenFaaS gateway binary that contains both code paths, and the Pi-side mode-switch script decides which one is active at runtime.

That means you do not need two separate gateway image builds for vanilla and prototype. Build and import the custom gateway image once, then switch the active runtime mode on the Pi only with:
- `benchmarks/micro/micro-bench2-initale-time/scripts/pi_enable_vanilla_gateway.sh`
- `benchmarks/micro/micro-bench2-initale-time/scripts/pi_enable_proto_gateway.sh`

For the prototype worker image, use the same persistent buildx builder as the gateway build. The prototype worker Dockerfile is arranged so the expensive `wolfssl` and `libtlspeek` layers match the gateway build and can be reused from the builder cache.

Recommended pattern:

```bash
export BUILDER_NAME=bench2-arm64-builder
```

Then build the gateway first, and build the prototype worker with the same builder:

```bash
BUILD_PROGRESS=plain BUILDER_NAME=${BUILDER_NAME} \
  bash benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_proto_gateway.sh

BUILD_PROGRESS=plain BUILDER_NAME=${BUILDER_NAME} \
  bash benchmarks/micro/micro-bench2-initale-time/scripts/build_push_proto_function_image.sh
```

When those two commands use the same persistent buildx builder, the worker build should reuse the gateway's cached wolfSSL/libtlspeek layers instead of recompiling them from zero.

## 6. Quick re-run — Vanilla direct HTTPS

Run from the laptop/workstation:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

bash benchmarks/micro/micro-bench2-initale-time/scripts/sync_code_to_pi.sh

docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench2-initale-time/function/timing_fn/Dockerfile \
  -t ttl.sh/timing-fn-bench2-initale:24h \
  --push .

ssh -t ${PI_SSH} \
  'export OPENFAAS_URL=http://127.0.0.1:8080; sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin'

ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && export OPENFAAS_URL=http://127.0.0.1:8080 && faas-cli deploy -f benchmarks/micro/micro-bench2-initale-time/function/stack.yml'

BUILD_PROGRESS=plain PI_SSH=${PI_SSH} \
  bash benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_vanilla_gateway.sh

curl -k https://${PI_IP}:8443/function/timing-fn

python3 benchmarks/micro/micro-bench2-initale-time/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 8443 \
  --out benchmarks/micro/micro-bench2-initale-time/client/vanilla_results.csv \
  --requests 50
```

Notes:
- by default the client generates a linear payload ladder from `32 KiB` to `1024 KiB` with a fixed `32 KiB` step
- use `--start-kb`, `--end-kb`, and `--step-kb` to try other linear steps

## 7. Quick re-run — Prototype rdtsc path

Run from the laptop/workstation:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

bash benchmarks/micro/micro-bench2-initale-time/scripts/sync_code_to_pi.sh

docker buildx build --platform linux/arm64 \
  --builder ${BUILDER_NAME:-bench2-arm64-builder} \
  -f benchmarks/micro/micro-bench2-initale-time/proto_function/timing-fn/Dockerfile \
  -t ttl.sh/timing-fn-tlsmigrate-bench2:24h \
  --push .

BUILD_PROGRESS=plain PI_SSH=${PI_SSH} \
  bash benchmarks/micro/micro-bench2-initale-time/scripts/build_copy_enable_proto_gateway.sh

bash prototype/faasd_tlsmigrate/provider/build_faasd_arm64.sh
PI_SSH=${PI_SSH} bash prototype/faasd_tlsmigrate/provider/pi_install_proto_provider.sh

ssh -t ${PI_SSH} \
  'export OPENFAAS_URL=http://127.0.0.1:8080; sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin'

ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && export OPENFAAS_URL=http://127.0.0.1:8080 && faas-cli deploy -f benchmarks/micro/micro-bench2-initale-time/proto_function/stack.yml && faas-cli list'

curl -k https://${PI_IP}:9443/function/timing-fn

python3 benchmarks/micro/micro-bench2-initale-time/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 9443 \
  --out benchmarks/micro/micro-bench2-initale-time/client/proto_results.csv \
  --requests 50
```

## 8. Generate plots with a fixed linear x-axis

Default linear windows:

```bash
python3 benchmarks/micro/micro-bench2-initale-time/client/plot_vanilla_vs_proto.py \
  --metric delta \
  --window-start-kb 32 \
  --x-step-kb 32 \
  --window-points 16 \
  --no-display
```

That generates several curve files when the payload range is too wide for one figure.
For example, with `--x-step-kb 32 --window-points 16`, the script creates one figure for the first 16 fixed-step x-axis points, then another for the next 16 points, and so on.

Try a different fixed step:

```bash
python3 benchmarks/micro/micro-bench2-initale-time/client/plot_vanilla_vs_proto.py \
  --metric delta \
  --window-start-kb 32 \
  --x-step-kb 64 \
  --window-points 12 \
  --curve-output benchmarks/micro/micro-bench2-initale-time/client/time_mean_vs_size_step64.png \
  --no-display
```

Histogram at one payload size:

```bash
python3 benchmarks/micro/micro-bench2-initale-time/client/plot_vanilla_vs_proto.py \
  --metric delta \
  --hist-payload $((512 * 1024)) \
  --hist-output benchmarks/micro/micro-bench2-initale-time/client/time_histogram_512KiB.png \
  --no-display
```

Main artifacts:
- `benchmarks/micro/micro-bench2-initale-time/client/vanilla_results.csv`
- `benchmarks/micro/micro-bench2-initale-time/client/proto_results.csv`
- one or more `time_mean_vs_size*.png` files, split by linear x-axis window
- histogram PNG selected with `--hist-output`

## 9. Restore back to the normal faasd gateway

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

ssh -t ${PI_SSH} \
  'cd ~/Prototype_sendfd && bash benchmarks/micro/micro-bench2-initale-time/scripts/pi_restore_vanilla_gateway.sh'

PI_SSH=${PI_SSH} bash prototype/faasd_tlsmigrate/provider/pi_restore_vanilla_provider.sh
```

## 10. Notes

- `vanilla_proxy/` is kept only because this benchmark was cloned from bench 1. It is not used anymore in bench 2.
- the direct-HTTPS vanilla path and the prototype path are not the same behaviorally: vanilla executes the direct wolfSSL HTTPS path, while prototype executes the tlsmigrate + `tls_read_peek()` + sendfd path
- what is shared is only the built custom gateway image artifact, because that image contains both code paths and the Pi-side compose/env settings choose which one is enabled
- the provider patch is still needed only for the prototype mode
