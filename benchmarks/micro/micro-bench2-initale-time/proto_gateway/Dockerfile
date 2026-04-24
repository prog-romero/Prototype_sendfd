# syntax=docker/dockerfile:1

# Builds a custom OpenFaaS Gateway image (from openfaas/faas) with an
# additional TLS-migration listener on 9443 using wolfSSL + libtlspeek + sendfd.
#
# This image is intended for the PROTOTYPE scenario only.

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

# Bring in the custom wolfSSL fork + libtlspeek from this repo.
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

# 3) Clone openfaas/faas and apply our gateway patch
COPY benchmarks/micro/micro-bench2-initale-time/proto_gateway/patches/openfaas-faas-gateway-tlsmigrate.patch /src/gateway-tlsmigrate.patch
COPY benchmarks/micro/micro-bench2-initale-time/proto_gateway/patches/openfaas-faas-gateway-bench2-vanilla-https.patch /src/gateway-bench2-vanilla-https.patch

RUN git clone https://github.com/openfaas/faas /src/faas \
 && cd /src/faas \
 && git checkout "${FAAS_COMMIT}" \
 && git apply --recount /src/gateway-tlsmigrate.patch \
 && git apply --recount /src/gateway-bench2-vanilla-https.patch

# 4) Build the gateway with CGO enabled (wolfSSL + libtlspeek)
WORKDIR /src/faas/gateway

ENV CGO_ENABLED=1

RUN --mount=type=cache,target=/go/pkg/mod,sharing=locked \
   --mount=type=cache,target=/root/.cache/go-build,sharing=locked \
   go mod download
RUN --mount=type=cache,target=/go/pkg/mod,sharing=locked \
   --mount=type=cache,target=/root/.cache/go-build,sharing=locked \
   go build -o /out/gateway .


FROM debian:bookworm-slim AS ship

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
   --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
   apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 10001 app

WORKDIR /home/app

# Runtime deps for CGO-linked wolfSSL
COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/gateway /home/app/gateway
COPY --from=build /src/faas/gateway/assets /home/app/assets

# Self-signed cert used for the prototype HTTPS listener.
COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key
RUN chown app:app /certs/server.crt /certs/server.key \
 && chmod 0644 /certs/server.crt \
 && chmod 0600 /certs/server.key

EXPOSE 8080
EXPOSE 8082
EXPOSE 9443

USER app

CMD ["./gateway"]
