# syntax=docker/dockerfile:1
#
# Dockerfile for sumprod-vanilla-fn-b (integration bench, sum & product)
#

# ── Stage 1: build fwatchdog ──────────────────────────────────────────────────
FROM golang:latest AS watchdog-build

WORKDIR /go/src/github.com/openfaas/of-watchdog

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/of-watchdog/ .

RUN rm -rf vendor && go mod download

RUN mkdir -p /out && CGO_ENABLED=0 GOOS=linux \
    go build -ldflags "-s -w" -o /out/fwatchdog .

# ── Stage 2: build the C vanilla function worker ──────────────────────────────
FROM golang:1.24-bookworm AS fn-build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/sumprod-vanilla-fn-b/vanilla_fn_worker.c /src/

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

ENV fprocess="/usr/local/bin/vanilla-fn-worker"
ENV mode=http
ENV http_upstream_url="http://127.0.0.1:8085"
ENV sendfd_enable=0

ENV VANILLA_FN_NAME=vanilla-fn-b
ENV VANILLA_FN_PORT=8085

CMD ["/usr/bin/fwatchdog"]
