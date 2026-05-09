FROM debian:bookworm AS build

WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY benchmarks/micro/micro-bench-max-throughput-http/function/timing-fn-b/timing_fn.c /src/timing_fn.c
RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing_fn /src/timing_fn.c

FROM debian:bookworm-slim
COPY --from=build /out/timing_fn /timing_fn
EXPOSE 8080
ENTRYPOINT ["/timing_fn"]
