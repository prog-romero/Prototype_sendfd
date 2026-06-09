# syntax=docker/dockerfile:1
#
# Dockerfile for vanilla-fn-a (integration bench, vanilla proxy timing)
#
# Two binaries run inside the container:
#   1. fwatchdog (PID 1) — our modified of-watchdog in http mode.
#      It starts vanilla-fn-worker as fprocess and reverse-proxies every
#      request to it on 127.0.0.1:8080.  The X-Top1-Rdtsc header injected
#      by the gateway passes through transparently.
#   2. vanilla-fn-worker — the C HTTP timing server.
#      Reads X-Top1-Rdtsc (top1), stamps top2 after full body recv,
#      returns JSON timing data in the same schema as the prototype worker.
#
# Build context must be the repository root.  Example:
#   docker build \
#     -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/vanilla-fn-a/Dockerfile \
#     -t romerosdd/vanilla-fn-a-ka-integration:latest .

# ── Stage 1: build fwatchdog (same modified of-watchdog as prototype) ────────
FROM golang:latest AS watchdog-build

WORKDIR /go/src/github.com/openfaas/of-watchdog

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/of-watchdog/ .

RUN rm -rf vendor && go mod download

RUN mkdir -p /out && CGO_ENABLED=0 GOOS=linux \
    go build -ldflags "-s -w" -o /out/fwatchdog .

# ── Stage 2: build the C vanilla function worker ──────────────────────────────
# Use the same golang:bookworm base as the prototype so gcc is available
# via build-essential without pulling a separate gcc image.
FROM golang:1.24-bookworm AS fn-build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/vanilla-fn-a/vanilla_fn_worker.c /src/

RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/vanilla-fn-worker \
    /src/vanilla_fn_worker.c \
    -lpthread

# ── Stage 3: final runtime image ─────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=watchdog-build /out/fwatchdog             /usr/bin/fwatchdog
COPY --from=fn-build       /out/vanilla-fn-worker     /usr/local/bin/vanilla-fn-worker

# of-watchdog settings — http mode: watchdog starts fprocess and reverse-proxies
ENV fprocess="/usr/local/bin/vanilla-fn-worker"
ENV mode=http
ENV http_upstream_url="http://127.0.0.1:8085"
ENV sendfd_enable=0

# Function-specific settings (read by the C worker)
ENV VANILLA_FN_NAME=vanilla-fn-a
ENV VANILLA_FN_PORT=8085

# fwatchdog is PID 1; it starts the C worker as fprocess
CMD ["/usr/bin/fwatchdog"]
