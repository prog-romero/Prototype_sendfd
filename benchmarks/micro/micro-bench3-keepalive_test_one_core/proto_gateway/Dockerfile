#
# bench3 keepalive gateway image (arm64).
#
# Build from workspace root:
#   docker buildx build \
#     --platform linux/arm64 \
#     -f benchmarks/micro/micro-bench3-keepalive/proto_gateway/Dockerfile \
#     -t docker.io/local/bench3-keepalive-gateway:arm64 \
#     --load .
#
# What this image contains:
#   - wolfSSL (with --enable-atomicuser --enable-sessionexport --enable-tls13 ...)
#   - libtlspeek
#   - openfaas/faas gateway @ pinned commit, patched to start bench2gw
#   - bench2gw Go/CGO package (C shim + Go server) for the prototype path
#   - benchhttps Go/CGO package for the direct vanilla HTTPS path inside the
#     gateway process itself (same architecture as bench2-initale-time)
#
# Environment variables (set in docker-compose.yaml on the Pi):
#   BENCH2GW_ENABLE=1
#   BENCH2GW_LISTEN=:9444
#   BENCH2GW_CERT=/certs/server.crt
#   BENCH2GW_KEY=/certs/server.key
#   BENCH2GW_SOCKET_DIR=/run/bench2
#   BENCH2GW_RELAY_SOCKET=/run/bench2/relay.sock
#   BENCH2GW_PEEK_BYTES=8192
#
#   BENCH2_TLS_ENABLE=1
#   BENCH2_TLS_LISTEN=:8444
#   BENCH2_TLS_CERT=/certs/server.crt
#   BENCH2_TLS_KEY=/certs/server.key

FROM golang:1.24-bookworm AS build

ARG FAAS_COMMIT=f4dc39f8d87bf666fd5702791d752b382d90163b
ARG BENCH2_DEBUG=0

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates \
    build-essential \
    autoconf automake libtool \
    cmake pkg-config \
 && rm -rf /var/lib/apt/lists/*

# ── wolfSSL (enable-atomicuser required for GetClientWriteKey etc.) ────────
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

# ── libtlspeek ────────────────────────────────────────────────────────────
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

# ── bench3 common headers and gateway add-on packages ─────────────────────
COPY benchmarks/micro/micro-bench3-keepalive/common/ /src/common/
COPY benchmarks/micro/micro-bench3-keepalive/common/ /src/bench2common/
COPY benchmarks/micro/micro-bench3-keepalive/proto_gateway/bench2gw/ /src/bench2gw/
COPY benchmarks/micro/micro-bench3-keepalive/proto_gateway/benchhttps/ /src/benchhttps/

# Make common headers available to the CGO compilation in the build context
RUN cp /src/bench2common/bench2_rdtsc.h  /src/bench2gw/bench2_rdtsc.h \
 && cp /src/bench2common/bench2_payload.h /src/bench2gw/bench2_payload.h

RUN install -d /out

# ── Clone and patch openfaas/faas gateway ────────────────────────────────
RUN git clone https://github.com/openfaas/faas /src/faas \
 && cd /src/faas \
 && git checkout "${FAAS_COMMIT}" \
 && perl -0pi -e 's|"net/http"\n\t"time"|"net/http"\n\t"os"\n\t"time"|' \
      gateway/main.go \
 && perl -0pi -e 's|"github.com/openfaas/faas/gateway/scaling"\n|"github.com/openfaas/faas/gateway/scaling"\n\t"github.com/openfaas/faas/gateway/bench2gw"\n\t"github.com/openfaas/faas/gateway/benchhttps"\n|' \
      gateway/main.go \
 && perl -0pi -e 's|go runMetricsServer\(\)\n\n|go runMetricsServer()\n\n\tif os.Getenv("BENCH2GW_ENABLE") == "1" {\n\t\tbench2gw.MaybeStartFromEnv()\n\t}\n\n\tif os.Getenv("BENCH2_TLS_ENABLE") == "1" {\n\t\tbenchhttps.MaybeStartFromEnv(r)\n\t}\n\n|' \
      gateway/main.go \
 && mkdir -p /src/faas/gateway/bench2gw \
 && cp -a /src/bench2gw/. /src/faas/gateway/bench2gw/ \
 && mkdir -p /src/faas/gateway/benchhttps \
 && cp -a /src/benchhttps/. /src/faas/gateway/benchhttps/

WORKDIR /src/faas/gateway

ENV CGO_ENABLED=1

RUN --mount=type=cache,target=/go/pkg/mod,sharing=locked \
    --mount=type=cache,target=/root/.cache/go-build,sharing=locked \
    go mod download

RUN --mount=type=cache,target=/go/pkg/mod,sharing=locked \
    --mount=type=cache,target=/root/.cache/go-build,sharing=locked \
    go build -o /out/gateway .

# ── Ship stage ───────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS ship

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
     ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 10001 app

WORKDIR /home/app

COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/gateway /home/app/gateway
COPY --from=build /src/faas/gateway/assets /home/app/assets

COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key
RUN chown app:app /home/app/gateway \
 && chmod 0755 /home/app/gateway \
 && chown app:app /certs/server.crt /certs/server.key \
 && chmod 0644 /certs/server.crt \
 && chmod 0600 /certs/server.key

EXPOSE 8080
EXPOSE 8082
EXPOSE 8444
EXPOSE 9444

USER app
CMD ["./gateway"]
