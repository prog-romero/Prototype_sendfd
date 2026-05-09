# syntax=docker/dockerfile:1

FROM golang:1.24-bookworm AS build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench3-keepalive-http/common/bench2_rdtsc.h /src/bench2_rdtsc.h
COPY benchmarks/micro/micro-bench3-keepalive-http/proto_function/timing-fn-b/timing_fn_ka_worker.c /src/timing_fn_ka_worker.c
COPY libtlspeek/common/*.h libtlspeek/common/*.c /src/

RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing-fn-ka-worker \
    /src/timing_fn_ka_worker.c \
    /src/sendfd.c \
    /src/unix_socket.c \
    -I/src \
    -lpthread

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/timing-fn-ka-worker /usr/local/bin/timing-fn-ka-worker

ENTRYPOINT ["/usr/local/bin/timing-fn-ka-worker"]
