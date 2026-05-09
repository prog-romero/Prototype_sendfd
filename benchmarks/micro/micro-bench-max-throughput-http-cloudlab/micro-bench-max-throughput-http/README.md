# micro-bench2-initial-http — Scenario A with HTTP top1/top2

This benchmark is the plain-HTTP version of `micro-bench2-initale-time`.

It keeps the same benchmark contract:

- one new connection per request
- `Connection: close`
- `top1_rdtsc`, `top2_rdtsc`, `delta_cycles`, `cntfrq`, `delta_ns`
- `top2` stamped only after the target function or worker has fully consumed the request body

The difference is the transport:

- vanilla uses a patched faasd gateway that accepts plain HTTP directly on `:8082`
- prototype uses a patched faasd gateway that accepts plain HTTP on `:8083`, does `MSG_PEEK`, then migrates the accepted socket to the function container with `sendfd`
- no wolfSSL and no `libtlspeek` are used in this benchmark

Vanilla and prototype are still meant to be run one after the other, not at the same time.

## 1. What is measured

Condition:

- one new HTTP connection per request
- `Connection: close`
- same Raspberry Pi machine for gateway/provider/function, so the architectural counter is shared

Measured fields:

- `top1_rdtsc`: benchmark start stamp
- `top2_rdtsc`: benchmark stop stamp
- `delta_cycles`: `top2_rdtsc - top1_rdtsc`
- `cntfrq`: architectural counter frequency used to convert cycles to time
- `delta_ns`: nanosecond conversion of `delta_cycles`
- `client_rtt_ns`: end-to-end client-side request time

Start and stop points:

- vanilla:
  - `top1`: request bytes are already in the gateway kernel receive buffer and the benchmark HTTP listener is just about to do the first real consuming read before forwarding into the normal faasd HTTP path
  - `top2`: the vanilla function container has fully consumed the forwarded HTTP request body
- prototype:
  - `top1`: request bytes are already in the gateway kernel receive buffer and the patched gateway is just about to do `MSG_PEEK`
  - the gateway peeks only enough request data to identify `/function/<name>` without consuming the socket
  - `top2`: the prototype worker has fully consumed the HTTP request body directly from the migrated client socket

## 2. Pipelines under test

### Vanilla

Path:

`client (HTTP)` -> `patched faasd gateway :8082` -> `normal faasd HTTP forwarding on :8080` -> `timing-fn (HTTP, C container)`

Key points:

- faasd still handles normal HTTP forwarding
- the benchmark listener stamps `top1` before the first consuming read
- the benchmark listener injects `X-Bench2-Top1-Rdtsc` and `X-Bench2-Cntfrq`
- the function container stamps `top2` after the full body read

### Prototype

Path:

`client (HTTP)` -> `patched faasd gateway :8083` -> `MSG_PEEK` -> `sendfd(SCM_RIGHTS)` -> `timing-fn worker (C container, direct socket I/O)`

Key points:

- the gateway waits until the socket is readable
- `top1` is stamped immediately before `MSG_PEEK`
- the gateway routes by parsing `/function/<name>` from the peeked request line
- the worker performs the first consuming read on the original client socket

## 3. Files involved

Main benchmark files:

- `benchmarks/micro/micro-bench2-initial-http/client/run_payload_sweep.py`
- `benchmarks/micro/micro-bench2-initial-http/function/timing_fn/timing_fn.c`
- `benchmarks/micro/micro-bench2-initial-http/proto_function/timing-fn/timing_fn_worker.c`
- `benchmarks/micro/micro-bench2-initial-http/common/bench2_rdtsc.h`

Gateway and mode-switch helpers:

- `benchmarks/micro/micro-bench2-initial-http/proto_gateway/`
- `benchmarks/micro/micro-bench2-initial-http/scripts/build_copy_enable_vanilla_gateway.sh`
- `benchmarks/micro/micro-bench2-initial-http/scripts/pi_enable_vanilla_gateway.sh`
- `benchmarks/micro/micro-bench2-initial-http/scripts/build_copy_enable_proto_gateway.sh`
- `benchmarks/micro/micro-bench2-initial-http/scripts/pi_enable_proto_gateway.sh`
- `benchmarks/micro/micro-bench2-initial-http/scripts/pi_restore_vanilla_gateway.sh`

## 4. Quick re-run — Vanilla HTTP

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

bash benchmarks/micro/micro-bench2-initial-http/scripts/prepare_vanilla_stack.sh

curl -sS http://${PI_IP}:8082/function/timing-fn

python3 benchmarks/micro/micro-bench2-initial-http/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 8082 \
  --out benchmarks/micro/micro-bench2-initial-http/results/vanilla_http.csv \
  --requests 50
```

## 5. Quick re-run — Prototype HTTP

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

bash benchmarks/micro/micro-bench2-initial-http/scripts/prepare_proto_stack.sh

curl -sS http://${PI_IP}:8083/function/timing-fn

python3 benchmarks/micro/micro-bench2-initial-http/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 8083 \
  --out benchmarks/micro/micro-bench2-initial-http/results/proto_http.csv \
  --requests 50
```

## 6. Notes

- `:8080` remains the normal faasd management and forwarding endpoint
- `:8082` is the vanilla benchmark entrypoint
- `:8083` is the prototype benchmark entrypoint
- both listeners are implemented inside the same custom gateway image, and the Pi-side mode-switch scripts decide which benchmark mode is active
