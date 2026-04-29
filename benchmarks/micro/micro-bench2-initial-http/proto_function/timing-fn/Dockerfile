# syntax=docker/dockerfile:1

FROM golang:1.24-bookworm AS build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench2-initial-http/common/bench2_rdtsc.h /src/bench2_rdtsc.h
COPY benchmarks/micro/micro-bench2-initial-http/proto_function/timing-fn/timing_fn_worker.c /src/timing_fn_worker.c
COPY libtlspeek/common/*.h libtlspeek/common/*.c /src/

RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing-fn-worker \
    /src/timing_fn_worker.c \
    /src/sendfd.c \
    /src/unix_socket.c \
    -I/src \
    -lpthread

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/timing-fn-worker /usr/local/bin/timing-fn-worker

# 🔥 CRITICAL: keep container alive + run your worker
ENTRYPOINT ["/usr/local/bin/timing-fn-worker"]