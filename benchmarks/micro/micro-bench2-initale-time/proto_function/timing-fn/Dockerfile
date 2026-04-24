# syntax=docker/dockerfile:1

# Prototype timing-fn worker image.
# Listens on /run/tlsmigrate/timing-fn.sock, receives fd+TLS state from the
# patched gateway (9443), restores TLS, reads HTTP request+body, and replies
# directly to the client with the same JSON fields as the vanilla Go function.
#
# Important: the first half of this build intentionally mirrors the gateway
# Dockerfile so a persistent buildx builder can reuse the expensive wolfSSL and
# libtlspeek layers instead of recompiling them for the worker.

FROM golang:1.24-bookworm AS build

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
   git \
    ca-certificates \
    build-essential \
    autoconf automake libtool \
    cmake pkg-config \
 && rm -rf /var/lib/apt/lists/*

COPY wolfssl/ /src/wolfssl/
COPY libtlspeek/ /src/libtlspeek/

# 1) Build & install wolfSSL (needs the custom migration symbols)
RUN cd /src/wolfssl \
 && ./autogen.sh \
 && ./configure \
    --enable-tls13 \
    --enable-opensslextra \
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

# 2) Build libtlspeek static library and install headers
RUN cd /src/libtlspeek \
 && rm -rf build \
 && mkdir -p build \
 && cd build \
 && cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DWOLFSSL_INCLUDE=/usr/local/include \
      -DWOLFSSL_LIB=/usr/local/lib/libwolfssl.so \
 && make -j"$(nproc)" tlspeek \
 && install -d /usr/local/include/tlspeek \
 && cp -f ../lib/*.h ../common/*.h /usr/local/include/tlspeek/ \
 && cp -f libtlspeek.a /usr/local/lib/libtlspeek.a \
 && ldconfig

# 3) Build the timing-fn worker
COPY benchmarks/micro/micro-bench2-initale-time/common/bench2_rdtsc.h /src/bench2_rdtsc.h
COPY benchmarks/micro/micro-bench2-initale-time/proto_function/timing-fn/timing_fn_worker.c /src/timing_fn_worker.c
RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing-fn-worker \
    /src/timing_fn_worker.c \
    -I/usr/local/include \
    -L/usr/local/lib \
    -ltlspeek -lwolfssl -lpthread


FROM debian:bookworm-slim AS ship

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Runtime deps for wolfSSL
COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/timing-fn-worker /usr/local/bin/timing-fn-worker

# Reuse the same self-signed cert as the gateway image (optional for import path)
COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key

CMD ["/usr/local/bin/timing-fn-worker"]
