# syntax=docker/dockerfile:1
#
# Dockerfile for timing-fn-a (integration bench, keepalive HTTP)
#
# Two binaries run inside the container:
#   1. fwatchdog (PID 1) — our modified of-watchdog.
#      It starts the sendfd relay goroutine (StartSendFDServer) which
#      listens on /run/tlsmigrate/<ip>.sock and relays [clientFD, pipeWriteFD]
#      to the C worker's /run/tlsmigrate/<ip>-fn.sock.
#   2. timing-fn-ka-worker — the C function process started by fwatchdog
#      via the "fprocess" env var.  It listens on <ip>-fn.sock and serves
#      HTTP directly on the raw TCP file descriptor.
#
# Build context must be the repository root.  Example:
#   docker build \
#     -f benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-a/Dockerfile \
#     -t timing-fn-a-ka-integration:latest .

# ── Stage 1: build fwatchdog (modified of-watchdog with sendfd relay) ────────
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

COPY benchmarks/micro/micro-bench3-keepalive-http-integration/proto_function/timing-fn-a/timing_fn_ka_worker.c /src/timing_fn_ka_worker.c
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

# of-watchdog settings
ENV fprocess="/usr/local/bin/timing-fn-ka-worker"
ENV mode=streaming
ENV sendfd_enable=1
ENV sendfd_socket_dir=/run/tlsmigrate

# Function-specific settings (read by the C worker)
ENV HTTPMIGRATE_KA_FUNCTION_NAME=timing-fn-a
ENV SENDFD_SOCKET_DIR=/run/tlsmigrate

# fwatchdog is PID 1; it starts the C worker as fprocess
CMD ["/usr/bin/fwatchdog"]
