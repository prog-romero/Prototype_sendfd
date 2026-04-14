# micro-bench1-initale-time — Scenario A (Initial mode)

This benchmark measures the elemental time spent between the end of the TLS handshake and the moment the function worker starts handling the request.

The benchmark has been validated in both modes:
- Vanilla: `500/500` successful rows (`HTTP 200`)
- Prototype: `500/500` successful rows (`HTTP 200`)

Vanilla and prototype are meant to be run one after the other, not at the same time.

## 1. What is measured

Condition:
- One new TLS connection per request
- `Connection: close`
- Same Raspberry Pi machine for gateway/proxy/provider/function, so the same monotonic clock is used everywhere

Measured fields:
- `delta_header_ns`: time from the benchmark start point to the instant the worker has the HTTP headers available
- `delta_body_ns`: time from the benchmark start point to the instant the worker has fully consumed the request body
- `client_rtt_ns`: end-to-end client-side request time on the laptop/workstation

Start point in each mode:
- Vanilla: right after TLS handshake in the frontal TLS proxy, before forwarding to the real `faasd` gateway on `:8080`
- Prototype: right after TLS handshake in the patched gateway on `:9443`, before `tls_read_peek()`, TLS-state serialization, and `sendfd`

## 2. Pipelines under test

### Vanilla

Path:

`client (HTTPS)` -> `wolfSSL TLS proxy :8443` -> `faasd gateway :8080` -> `timing-fn (HTTP, C container)`

Key points:
- Real upstream `faasd` gateway is used
- TLS is terminated by the frontal proxy, not by the function
- The function container is a C HTTP server that reads headers and then drains the body

### Prototype

Path:

`client (HTTPS)` -> `patched faasd gateway :9443` -> `tls_read_peek()` -> `serialize TLS state + sendfd(SCM_RIGHTS)` -> `timing-fn worker (C container, direct TLS I/O)`

Key points:
- Patched `faasd` provider mounts `/run/tlsmigrate` into function containers
- Patched gateway uses `wolfSSL + libtlspeek`
- Worker verifies ownership with worker-side `tls_read_peek()`, restores TLS state, then reads headers and body directly on the migrated TLS socket

## 3. Files involved

Main benchmark files:
- `benchmarks/micro/micro-bench1-initale-time/client/run_payload_sweep.py`
- `benchmarks/micro/micro-bench1-initale-time/client/plot_vanilla_vs_proto.py`
- `benchmarks/micro/micro-bench1-initale-time/function/timing_fn/timing_fn.c`
- `prototype/faasd_tlsmigrate/function/timing-fn/timing_fn_worker.c`

Prototype infrastructure:
- `prototype/faasd_tlsmigrate/gateway/`
- `prototype/faasd_tlsmigrate/provider/`
- `prototype/faasd_tlsmigrate/scripts/build_copy_enable_proto_gateway.sh`
- `prototype/faasd_tlsmigrate/scripts/pi_enable_proto_gateway.sh`
- `prototype/faasd_tlsmigrate/scripts/pi_restore_vanilla_gateway.sh`

## 4. One-time prerequisites

On the Pi:
- `faasd` installed and working
- `faas-cli` installed and logged in at least once
- repository cloned at `~/Prototype_sendfd`

Quick health check on the Pi:

```bash
curl -sS http://127.0.0.1:8080/healthz
```

If you need to sync files from the laptop to the Pi:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

ssh ${PI_SSH} 'mkdir -p ~/Prototype_sendfd/prototype ~/Prototype_sendfd/benchmarks/micro'

rsync -az --delete prototype/faasd_tlsmigrate/ \
  ${PI_SSH}:~/Prototype_sendfd/prototype/faasd_tlsmigrate/

rsync -az --delete benchmarks/micro/micro-bench1-initale-time/ \
  ${PI_SSH}:~/Prototype_sendfd/benchmarks/micro/micro-bench1-initale-time/
```

## 5. Quick re-run — Vanilla

Run from the laptop/workstation:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

# Sync the benchmark folder
rsync -az --delete benchmarks/micro/micro-bench1-initale-time/ \
  ${PI_SSH}:~/Prototype_sendfd/benchmarks/micro/micro-bench1-initale-time/

# Build and push the function image (expires after 24h on ttl.sh)
cd benchmarks/micro/micro-bench1-initale-time/function/timing_fn
docker buildx build --platform linux/arm64 -t ttl.sh/timing-fn-bench:24h --push .
cd -

# First time only if needed
ssh -t ${PI_SSH} \
  'export OPENFAAS_URL=http://127.0.0.1:8080; sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin'

# Deploy function
ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && export OPENFAAS_URL=http://127.0.0.1:8080 && faas-cli deploy -f benchmarks/micro/micro-bench1-initale-time/function/stack.yml'

# Start proxy in background
ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && nohup bash benchmarks/micro/micro-bench1-initale-time/vanilla_proxy/run_on_pi.sh >/tmp/tls_proxy.log 2>&1 < /dev/null &'

# Smoke test
curl -k https://${PI_IP}:8443/function/timing-fn

# Run sweep
python3 benchmarks/micro/micro-bench1-initale-time/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 8443 \
  --out benchmarks/micro/micro-bench1-initale-time/client/vanilla_results.csv \
  --requests 50
```

Expected result:
- `vanilla_results.csv` contains `500` rows with `http_status=200`

## 6. Quick re-run — Prototype

Run from the laptop/workstation:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

cd ~/Prototype_sendfd

# Sync benchmark and prototype files
ssh ${PI_SSH} 'mkdir -p ~/Prototype_sendfd/prototype ~/Prototype_sendfd/benchmarks/micro'
rsync -az --delete prototype/faasd_tlsmigrate/ \
  ${PI_SSH}:~/Prototype_sendfd/prototype/faasd_tlsmigrate/
rsync -az --delete benchmarks/micro/micro-bench1-initale-time/ \
  ${PI_SSH}:~/Prototype_sendfd/benchmarks/micro/micro-bench1-initale-time/

# Build and push the prototype function image (expires after 24h on ttl.sh)
docker buildx build --platform linux/arm64 \
  -f prototype/faasd_tlsmigrate/function/timing-fn/Dockerfile \
  -t ttl.sh/timing-fn-tlsmigrate:24h \
  --push .

# Build/copy/import/enable the prototype gateway locally on the Pi
# Enter the Pi sudo password when the helper reaches the import step.
BUILD_PROGRESS=plain PI_SSH=${PI_SSH} \
  bash prototype/faasd_tlsmigrate/scripts/build_copy_enable_proto_gateway.sh

# Build and install the patched provider on the Pi
bash prototype/faasd_tlsmigrate/provider/build_faasd_arm64.sh
PI_SSH=${PI_SSH} bash prototype/faasd_tlsmigrate/provider/pi_install_proto_provider.sh

# First time only if needed
ssh -t ${PI_SSH} \
  'export OPENFAAS_URL=http://127.0.0.1:8080; sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin'

# Deploy prototype function
ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && export OPENFAAS_URL=http://127.0.0.1:8080 && faas-cli deploy -f prototype/faasd_tlsmigrate/function/stack.yml && faas-cli list'

# Smoke test
curl -k https://${PI_IP}:9443/function/timing-fn

# Run sweep
python3 benchmarks/micro/micro-bench1-initale-time/client/run_payload_sweep.py \
  --host ${PI_IP} \
  --port 9443 \
  --out benchmarks/micro/micro-bench1-initale-time/client/proto_results.csv \
  --requests 50
```

Expected result:
- `proto_results.csv` contains `500` rows with `http_status=200`

## 7. If the gateway helper already copied the tar but stopped on sudo

If `build_copy_enable_proto_gateway.sh` reached `[scp]` and then stopped on the Pi import step, do not rebuild. Resume manually:

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

ssh -t ${PI_SSH} \
  'sudo ctr -n openfaas images import /tmp/faasd-gateway-tlsmigrate-arm64.tar && sudo ctr -n openfaas images ls | grep -F "docker.io/local/faasd-gateway-tlsmigrate:arm64"'

ssh -t ${PI_SSH} \
  'cd ~/Prototype_sendfd && PROTO_GATEWAY_IMAGE=docker.io/local/faasd-gateway-tlsmigrate:arm64 bash prototype/faasd_tlsmigrate/scripts/pi_enable_proto_gateway.sh'
```

## 8. Generate plots

Body completion plots:

```bash
python3 benchmarks/micro/micro-bench1-initale-time/client/plot_vanilla_vs_proto.py \
  --metric body \
  --no-display
```

Header-time plots:

```bash
python3 benchmarks/micro/micro-bench1-initale-time/client/plot_vanilla_vs_proto.py \
  --metric header \
  --curve-output benchmarks/micro/micro-bench1-initale-time/client/header_time_mean_vs_size.png \
  --hist-output benchmarks/micro/micro-bench1-initale-time/client/header_time_histogram.png \
  --no-display
```

Generated artifacts:
- `benchmarks/micro/micro-bench1-initale-time/client/vanilla_results.csv`
- `benchmarks/micro/micro-bench1-initale-time/client/proto_results.csv`
- `benchmarks/micro/micro-bench1-initale-time/client/time_mean_vs_size.png`
- `benchmarks/micro/micro-bench1-initale-time/client/time_histogram.png`
- `benchmarks/micro/micro-bench1-initale-time/client/header_time_mean_vs_size.png`
- `benchmarks/micro/micro-bench1-initale-time/client/header_time_histogram.png`

## 9. Restore back to vanilla

```bash
export PI_IP=192.168.2.2
export PI_SSH=romero@${PI_IP}

ssh -t ${PI_SSH} \
  'cd ~/Prototype_sendfd && bash prototype/faasd_tlsmigrate/scripts/pi_restore_vanilla_gateway.sh'

PI_SSH=${PI_SSH} bash prototype/faasd_tlsmigrate/provider/pi_restore_vanilla_provider.sh
```

## 10. Troubleshooting

### `faasd` is down on the Pi

Check:

```bash
ssh ${PI_SSH} 'systemctl is-active faasd; systemctl status faasd --no-pager -l | sed -n "1,80p"'
```

### `curl -k https://<PI_IP>:9443/function/timing-fn` returns `Empty reply from server`

Usually the gateway is up but the prototype function is not ready. Re-deploy and check the replica count:

```bash
ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && export OPENFAAS_URL=http://127.0.0.1:8080 && faas-cli deploy -f prototype/faasd_tlsmigrate/function/stack.yml && faas-cli list'
```

Expected:
- `timing-fn` at `Replicas: 1`

### `faas-cli deploy` cannot pull the function image

The `faasd` provider always pulls function images from a registry. Rebuild and push the function image again:

```bash
docker buildx build --platform linux/arm64 \
  -f prototype/faasd_tlsmigrate/function/timing-fn/Dockerfile \
  -t ttl.sh/timing-fn-tlsmigrate:24h \
  --push .
```

and for vanilla:

```bash
cd benchmarks/micro/micro-bench1-initale-time/function/timing_fn
docker buildx build --platform linux/arm64 -t ttl.sh/timing-fn-bench:24h --push .
```

### The vanilla proxy says `Address already in use`

Stop the old proxy and relaunch:

```bash
ssh ${PI_SSH} 'pkill -f tls_proxy || true'
ssh ${PI_SSH} \
  'cd ~/Prototype_sendfd && nohup bash benchmarks/micro/micro-bench1-initale-time/vanilla_proxy/run_on_pi.sh >/tmp/tls_proxy.log 2>&1 < /dev/null &'
```

## 11. Local artifacts that should not be committed

The following are generated locally and are ignored by the root `.gitignore`:
- `.buildx-cache/`
- `dist/`
- benchmark CSV outputs
- benchmark plot images and PDFs
- `__pycache__/`
