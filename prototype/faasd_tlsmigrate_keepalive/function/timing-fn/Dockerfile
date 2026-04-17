# syntax=docker/dockerfile:1

FROM debian:bookworm AS build

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    autoconf automake libtool \
    cmake pkg-config \
 && rm -rf /var/lib/apt/lists/*

COPY wolfssl/ /src/wolfssl/
COPY libtlspeek/ /src/libtlspeek/
COPY prototype/faasd_tlsmigrate_keepalive/function/timing-fn/timing_fn_keepalive_worker.c /src/timing_fn_keepalive_worker.c
COPY prototype/faasd_tlsmigrate_keepalive/common/tlsmigrate_keepalive.h /src/tlsmigrate_keepalive.h

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

RUN install -d /out \
 && gcc -O2 -Wall -Wextra -o /out/timing-ka-worker \
    /src/timing_fn_keepalive_worker.c \
    -I/src \
    -I/usr/local/include \
    -L/usr/local/lib \
    -ltlspeek -lwolfssl -lpthread

FROM debian:bookworm-slim AS ship

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/timing-ka-worker /usr/local/bin/timing-ka-worker
COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key

CMD ["/usr/local/bin/timing-ka-worker"]