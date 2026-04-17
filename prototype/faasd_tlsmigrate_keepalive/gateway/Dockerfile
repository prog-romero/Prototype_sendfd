# syntax=docker/dockerfile:1

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

COPY wolfssl/ /src/wolfssl/
COPY libtlspeek/ /src/libtlspeek/
COPY prototype/faasd_tlsmigrate_keepalive/gateway/tlsmigratekeepalive/ /src/gatewaykeepalive/

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

RUN git clone https://github.com/openfaas/faas /src/faas \
 && cd /src/faas \
 && git checkout "${FAAS_COMMIT}" \
 && perl -0pi -e 's|"net/http"\n\t"time"|"net/http"\n\t"os"\n\t"time"|' gateway/main.go \
 && perl -0pi -e 's|"github.com/openfaas/faas/gateway/scaling"\n|"github.com/openfaas/faas/gateway/scaling"\n\t"github.com/openfaas/faas/gateway/tlsmigratekeepalive"\n|' gateway/main.go \
 && perl -0pi -e 's|go runMetricsServer\(\)\n\n|go runMetricsServer()\n\n\tif os.Getenv("TLSMIGRATE_KEEPALIVE_ENABLE") == "1" {\n\t\ttlsmigratekeepalive.MaybeStartFromEnv()\n\t}\n\n|' gateway/main.go \
 && mkdir -p /src/faas/gateway/tlsmigratekeepalive \
 && cp -a /src/gatewaykeepalive/. /src/faas/gateway/tlsmigratekeepalive/

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

COPY --from=build /usr/local/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /out/gateway /home/app/gateway
COPY --from=build /src/faas/gateway/assets /home/app/assets

COPY libtlspeek/certs/server.crt /certs/server.crt
COPY libtlspeek/certs/server.key /certs/server.key
RUN chown app:app /certs/server.crt /certs/server.key \
 && chmod 0644 /certs/server.crt \
 && chmod 0600 /certs/server.key

EXPOSE 8080
EXPOSE 8082
EXPOSE 9444

USER app

CMD ["./gateway"]