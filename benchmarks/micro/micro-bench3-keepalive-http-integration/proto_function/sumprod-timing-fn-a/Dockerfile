# syntax=docker/dockerfile:1
#
# Dockerfile for sumprod-timing-fn-a (integration bench, sum & product)
#

# ── Stage 1: build fwatchdog ──────────────────────────────────────────────────
FROM golang:latest AS watchdog-build

WORKDIR /go/src/github.com/openfaas/of-watchdog

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/of-watchdog/ .

RUN rm -rf vendor && go mod download

RUN mkdir -p /out && CGO_ENABLED=0 GOOS=linux \
    go build -ldflags "-s -w" -o /out/fwatchdog .

# ── Stage 2: build the C function worker ─────────────────────────────────────
FROM golang:1.24-bookworm AS fn-build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/sumprod-timing-fn-a/timing_fn_ka_worker.c /src/timing_fn_ka_worker.c
COPY libtlspeek/common/*.h libtlspeek/common/*.c /src/

RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing-fn-ka-worker \
    /src/timing_fn_ka_worker.c \
    /src/sendfd.c \
    /src/unix_socket.c \
    -I/src \
    -lpthread

# ── Stage 3: final runtime image ─────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=watchdog-build /out/fwatchdog             /usr/bin/fwatchdog
COPY --from=fn-build       /out/timing-fn-ka-worker   /usr/local/bin/timing-fn-ka-worker

ENV fprocess="/usr/local/bin/timing-fn-ka-worker"
ENV mode=http
ENV http_upstream_url="http://127.0.0.1:8085"
ENV sendfd_enable=1
ENV sendfd_socket_dir=/run/tlsmigrate

# Function-specific settings (read by the C worker)
ENV HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a
ENV SENDFD_SOCKET_DIR=/run/tlsmigrate

CMD ["/usr/bin/fwatchdog"]
