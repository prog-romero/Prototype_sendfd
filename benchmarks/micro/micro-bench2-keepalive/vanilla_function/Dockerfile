# Bench2 vanilla function image (arm64).
# Plain HTTP server — no TLS.  Timing: reads X-Bench2-Top1-Rdtsc from gateway,
# stamps top2 after full body read, returns JSON delta.
#
# Build from workspace root:
#   docker buildx build --platform linux/arm64 \
#     -f benchmarks/micro/micro-bench2-keepalive/vanilla_function/Dockerfile \
#     -t ttl.sh/bench2-vanilla-fn:24h --push .

FROM debian:bookworm AS build

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

# Mirror source tree so that #include "../common/bench2_rdtsc.h" resolves.
COPY benchmarks/micro/micro-bench2-keepalive/common/bench2_rdtsc.h /src/common/bench2_rdtsc.h
COPY benchmarks/micro/micro-bench2-keepalive/vanilla_function/bench2_vanilla_fn.c /src/vanilla_function/bench2_vanilla_fn.c

RUN install -d /out \
 && gcc -O2 -Wall -Wextra \
    -o /out/bench2_vanilla_fn \
    /src/vanilla_function/bench2_vanilla_fn.c

FROM debian:bookworm-slim AS ship

COPY --from=build /out/bench2_vanilla_fn /usr/local/bin/bench2_vanilla_fn

ENV BENCH2_LISTEN_PORT=8080

CMD ["/usr/local/bin/bench2_vanilla_fn"]
