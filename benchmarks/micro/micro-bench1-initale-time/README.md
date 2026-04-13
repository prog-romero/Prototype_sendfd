# micro-bench1-initale-time — Scenario A (Initial) elemental timing

This benchmark implements **Scenario A: Initial mode** from the meeting report.

## Goal
Measure the time between:
- **After TLS handshake** on the gateway side (handshake excluded), and
- The moment the **worker reads**:
  - **HTTP headers**, or
  - **Full payload** (for the payload-size sweep)

## Two versions

### 1) Vanilla (classic proxy)
- The **real faasd/OpenFaaS gateway** is used.
- **No migration**.
- TLS is handled in front of the gateway (wolfSSL termination), then the request is forwarded (payload-dependent cost).

### 2) Prototype (migration)
- A **patched upstream OpenFaaS gateway/provider** integrates **wolfSSL + libtlspeek**.
- After handshake: `tls_read_peek()` for routing, then **serialize TLS state + `sendfd` (SCM_RIGHTS)** to the selected worker.
- Worker restores TLS state and serves the request (payload should not be forwarded by the gateway).

## Prerequisites (Raspberry Pi)
- `faasd` must be installed and the gateway must be reachable on `http://127.0.0.1:8080/`.

Use the helper scripts:
- `pi_install_faasd.sh`
- `pi_verify_faasd.sh`

## Status

### Vanilla (ready)
The **vanilla Scenario A** pipeline is implemented:

`client (HTTPS, Connection: close)` → `wolfSSL TLS proxy` → `faasd gateway (HTTP :8080)` → `timing-fn (HTTP)`

The proxy injects `X-Bench-T0-Ns` (monotonic ns) **after TLS handshake**, and the function reports:
- `delta_header_ns`: proxy t0 → function handler entry ("headers read")
- `delta_body_ns`: proxy t0 → function finished consuming request body

### Prototype (pending)
Upstream gateway/provider patching + TLS migration workers are not wired here yet.

## Run — Vanilla Scenario A

### Quick re-run (copy/paste)

Run these commands from your laptop/workstation (the machine where you run the Python client and build/push the function image):

```bash
# Set your Pi IP once
export PI_IP=192.168.2.2

cd ~/Prototype_sendfd

# 1) (Recommended) keep Pi and laptop in sync
rsync -az --delete benchmarks/micro/micro-bench1-initale-time/ \
  romero@${PI_IP}:~/Prototype_sendfd/benchmarks/micro/micro-bench1-initale-time/

# 2) Build + push the function image (arm64) to a registry
# NOTE: this tag expires after 24h, so re-run this step when you re-run later.
cd benchmarks/micro/micro-bench1-initale-time/function/timing_fn
docker buildx build --platform linux/arm64 -t ttl.sh/timing-fn-bench:24h --push .
cd -

# 3) (First time only) login on the Pi (uses sudo, so allocate a TTY)
ssh -t romero@${PI_IP} \
  'export OPENFAAS_URL=http://127.0.0.1:8080; sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin'

# 4) Deploy (or rolling-update) the function on the Pi
ssh romero@${PI_IP} \
  'cd ~/Prototype_sendfd && faas-cli deploy -f benchmarks/micro/micro-bench1-initale-time/function/stack.yml'

# 5) Start the TLS proxy on the Pi (background)
ssh romero@${PI_IP} \
  'cd ~/Prototype_sendfd && nohup bash benchmarks/micro/micro-bench1-initale-time/vanilla_proxy/run_on_pi.sh >/tmp/tls_proxy.log 2>&1 < /dev/null &'

# 6) Run the payload sweep (writes the final CSV)
python3 benchmarks/micro/micro-bench1-initale-time/client/run_payload_sweep.py \
  --host "${PI_IP}" \
  --out benchmarks/micro/micro-bench1-initale-time/client/vanilla_results.csv \
  --requests 50

# 7) Inspect the result quickly
head -n 5 benchmarks/micro/micro-bench1-initale-time/client/vanilla_results.csv
```

Notes:
- If the proxy is already running and you see “Address already in use”, stop the existing `tls_proxy` process on the Pi, then re-run step 5.
- `ttl.sh/...:24h` expires automatically; if you re-run later and deploy fails to pull, just re-run step 2.

### (Recommended) Sync this benchmark folder to the Pi
If the Pi doesn't have this directory yet (it is currently untracked in git), copy it from your client machine:
- `cd Prototype_sendfd`
- `rsync -az --delete benchmarks/micro/micro-bench1-initale-time/ romero@<PI_IP>:~/Prototype_sendfd/benchmarks/micro/micro-bench1-initale-time/`

### 0) Verify faasd on the Pi
Run on the Pi:
- `cd ~/Prototype_sendfd && bash benchmarks/micro/micro-bench1-initale-time/pi_verify_faasd.sh`

### 1) Build + deploy the timing function
Build the container image on a machine with Docker (x86 is fine):
- `cd benchmarks/micro/micro-bench1-initale-time/function/timing_fn`
- `docker buildx build --platform linux/arm64 -t ttl.sh/timing-fn-bench:24h --push .`

Note: `faasd-provider` always pulls images from a registry during deploy/update, so importing the image into containerd with `ctr images import` is not sufficient.

Deploy with `faas-cli` on the Pi:
- `export OPENFAAS_URL=http://127.0.0.1:8080`
- `sudo cat /var/lib/faasd/secrets/basic-auth-password | faas-cli login -u admin --password-stdin`  # first time only
- `faas-cli deploy -f benchmarks/micro/micro-bench1-initale-time/function/stack.yml`

### 2) Run the wolfSSL TLS front-end proxy
Run on the Pi (from the repo clone):
- `bash benchmarks/micro/micro-bench1-initale-time/vanilla_proxy/run_on_pi.sh`

If you want to keep it running in the background:
- `nohup bash benchmarks/micro/micro-bench1-initale-time/vanilla_proxy/run_on_pi.sh >/tmp/tls_proxy.log 2>&1 < /dev/null &`

Defaults:
- Listens on `0.0.0.0:8443`
- Forwards to `127.0.0.1:8080`
- Uses certs from `libtlspeek/certs/`

### 3) Run the client payload sweep
Run on the client machine (your x86 laptop/desktop):
- `python3 benchmarks/micro/micro-bench1-initale-time/client/run_payload_sweep.py --host <PI_IP> --out benchmarks/micro/micro-bench1-initale-time/client/vanilla_results.csv --requests 50`

The CSV contains one line per request with `delta_header_ns` and `delta_body_ns`.
