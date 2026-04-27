# syntax=docker/dockerfile:1

#
# bench2 initial-time gateway image (arm64).
#
# Build from workspace root:
#   docker buildx build \
#     --platform linux/arm64 \
#     -f benchmarks/micro/micro-bench2-initale-time/proto_gateway/Dockerfile \
#     -t docker.io/local/faasd-gateway-bench2-initale:arm64 \
#     --load .
#
# What this image contains:
#   - wolfSSL (with --enable-sessionexport --enable-tls13 ...)
#   - libtlspeek
#   - openfaas/faas gateway @ pinned commit, patched for:
#       - prototype path (tlsmigrate + sendfd) on :9443
#       - vanilla wolfSSL HTTPS path (benchhttps module) on :8443
#
# Environment variables (set in docker-compose.yaml on the Pi):
#   TLSMIGRATE_ENABLE=1        -> activates the prototype path
#   BENCH2_TLS_ENABLE=1        -> activates the vanilla wolfSSL HTTPS path
#   BENCH2_TLS_LISTEN=:8443
#   BENCH2_TLS_CERT=/certs/server.crt
#   BENCH2_TLS_KEY=/certs/server.key
#   BENCH2_TLS_UPSTREAM=127.0.0.1:8080

FROM golang:1.24-bookworm AS build

ARG FAAS_COMMIT=f4dc39f8d87bf666fd5702791d752b382d90163b

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates \
    build-essential \
    autoconf automake libtool \
    cmake pkg-config \
 && rm -rf /var/lib/apt/lists/*

# ── wolfSSL ────────────────────────────────────────────────────────────────
COPY wolfssl/ /src/wolfssl/
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

# ── libtlspeek ────────────────────────────────────────────────────────────
COPY libtlspeek/ /src/libtlspeek/
RUN cd /src/libtlspeek \
 && rm -rf build && mkdir -p build && cd build \
 && cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DWOLFSSL_INCLUDE=/usr/local/include \
      -DWOLFSSL_LIB=/usr/local/lib/libwolfssl.so \
 && make -j"$(nproc)" tlspeek \
 && install -d /usr/local/include/tlspeek \
 && cp -f ../lib/*.h ../common/*.h /usr/local/include/tlspeek/ \
 && cp -f libtlspeek.a /usr/local/lib/libtlspeek.a \
 && ldconfig

# ── bench2 modules: vanilla wolfSSL HTTPS (benchhttps) ────────────────────
COPY benchmarks/micro/micro-bench2-initale-time/proto_gateway/benchhttps/ /src/benchhttps/

RUN install -d /out

# ── Bring in the prototype migration patch ────────────────────────────────
COPY benchmarks/micro/micro-bench2-initale-time/proto_gateway/patches/openfaas-faas-gateway-tlsmigrate.patch /src/gateway-tlsmigrate.patch

# ── Clone, patch and inject modules into the openfaas/faas gateway ─────────
RUN git clone https://github.com/openfaas/faas /src/faas \
 && cd /src/faas \
 && git checkout "${FAAS_COMMIT}" \
 && git apply --recount /src/gateway-tlsmigrate.patch \
 && perl -0pi -e 's|"net/http"\n\t"time"|"net/http"\n\t"os"\n\t"time"|' \
      gateway/main.go \
 && perl -0pi -e 's|"github.com/openfaas/faas/gateway/scaling"\n|"github.com/openfaas/faas/gateway/scaling"\n\t"github.com/openfaas/faas/gateway/benchhttps"\n|' \
      gateway/main.go \
 && perl -0pi -e 's|go runMetricsServer\(\)\n\n|go runMetricsServer()\n\n\tif os.Getenv("BENCH2_TLS_ENABLE") == "1" {\n\t\tbenchhttps.MaybeStartFromEnv(r)\n\t}\n\n|' \
      gateway/main.go \
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

# ── Ship stage ────────────────────────────────────────────────────────────
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
EXPOSE 8443
EXPOSE 9443

USER app
CMD ["./gateway"]
