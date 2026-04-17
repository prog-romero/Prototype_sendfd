#
# bench2 prototype worker image (arm64).
#
# Build from workspace root:
#   docker buildx build \
#     --platform linux/arm64 \
#     -f benchmarks/micro/micro-bench2-keepalive/proto_worker/Dockerfile \
#     -t ttl.sh/bench2-proto-worker:24h \
#     --load .
#
# Environment variables (set in stack.yml):
#   BENCH2_FUNCTION_NAME   e.g. "bench2-fn-a"
#   BENCH2_SOCKET_DIR      default /run/bench2
#   BENCH2_RELAY_SOCKET    default /run/bench2/relay.sock
#   BENCH2_CERT            default /certs/server.crt
#   BENCH2_KEY             default /certs/server.key

FROM debian:bookworm AS build

WORKDIR /src

ARG BENCH2_DEBUG=0

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    autoconf automake libtool \
    cmake pkg-config \
 && rm -rf /var/lib/apt/lists/*

# wolfSSL (--enable-atomicuser required for GetClientWriteKey etc.)
COPY wolfssl/ /src/wolfssl/
RUN cd /src/wolfssl \
 && ./autogen.sh \
 && ./configure \
    --enable-tls13 \
    --enable-opensslextra \
    --enable-atomicuser \
    --enable-sessionexport \
    --enable-session-ticket \
    --enable-keylog-export \
    --enable-aesgcm \
    --enable-chacha \
    --enable-hkdf \
    --enable-aescbc \
    --enable-static \
    --enable-shared \
 && make -j"$(nproc)" \
 && make install \
 && ldconfig

# libtlspeek
COPY libtlspeek/ /src/libtlspeek/
RUN cd /src/libtlspeek \
 && rm -rf build && mkdir -p build && cd build \
 && extra_cflags='' \
 && if [ "$BENCH2_DEBUG" = "1" ]; then extra_cflags='-DTLSPEEK_ENABLE_VERBOSE_LOGS'; fi \
 && cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$extra_cflags" \
      -DWOLFSSL_INCLUDE=/usr/local/include \
      -DWOLFSSL_LIB=/usr/local/lib/libwolfssl.so \
 && make -j"$(nproc)" tlspeek \
 && install -d /usr/local/include/tlspeek \
 && cp -f ../lib/*.h ../common/*.h /usr/local/include/tlspeek/ \
 && cp -f libtlspeek.a /usr/local/lib/libtlspeek.a \
 && ldconfig

# bench2 common headers and worker
COPY benchmarks/micro/micro-bench2-keepalive/common/bench2_rdtsc.h  /src/bench2_rdtsc.h
COPY benchmarks/micro/micro-bench2-keepalive/common/bench2_payload.h /src/bench2_payload.h
COPY benchmarks/micro/micro-bench2-keepalive/proto_worker/bench2_proto_worker.c /src/

RUN install -d /out \
 && extra_cflags='' \
 && if [ "$BENCH2_DEBUG" = "1" ]; then extra_cflags='-DBENCH2_WORKER_ENABLE_TRACE'; fi \
 && gcc -O2 -Wall -Wextra \
    $extra_cflags \
    -I/src \
    -I/usr/local/include \
    -L/usr/local/lib \
    -o /out/bench2_proto_worker \
    /src/bench2_proto_worker.c \
    -ltlspeek -lwolfssl -lpthread

FROM debian:bookworm-slim AS ship

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/bench2_proto_worker /usr/local/bin/bench2_proto_worker
COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key

CMD ["/usr/local/bin/bench2_proto_worker"]
