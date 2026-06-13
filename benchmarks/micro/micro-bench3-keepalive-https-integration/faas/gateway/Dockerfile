# syntax=docker/dockerfile:1
# ── Stage 1: Build wolfSSL from source ───────────────────────────────────────
# We build wolfSSL with the specific flags required by libtlspeek:
#   --enable-tls13            : TLS 1.3 support
#   --enable-keylog-export    : traffic key export (needed by tlspeek_keylog_cb)
#   --enable-opensslextra     : OpenSSL-compatible API (wolfSSL_GetClientWriteKey etc.)
#   --enable-session-export   : wolfSSL_tls_export / wolfSSL_tls_import
# Cross-compiled (built for ARM64 target platform).
#
# PLATFORM NOTE: build stages use $BUILDPLATFORM (your x86_64 machine) so the
# cross-compilation toolchain runs natively.  The runtime stage is TARGETPLATFORM
# (linux/arm64) so the ARM64 gateway binary can find its dynamic linker.
# Build command: docker buildx build --platform linux/arm64 ...

# $BUILDPLATFORM is automatically set by Docker Buildx to the builder's native arch.
ARG BUILDPLATFORM
FROM --platform=$BUILDPLATFORM golang:1.24-bookworm AS wolfssl-build

RUN apt-get update && apt-get install -y \
    build-essential autoconf automake libtool \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

COPY wolfssl/ /wolfssl-src/

WORKDIR /wolfssl-src
RUN ./autogen.sh && \
    CC=aarch64-linux-gnu-gcc \
    ./configure \
        --host=aarch64-linux-gnu \
        --prefix=/wolfssl-install \
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
        --enable-shared \
        --enable-static && \
    make -j$(nproc) && \
    make install

# ── Stage 2: Build libtlspeek ─────────────────────────────────────────────────
FROM wolfssl-build AS tlspeek-build

COPY libtlspeek /libtlspeek-src

RUN apt-get update && apt-get install -y cmake && rm -rf /var/lib/apt/lists/*

WORKDIR /libtlspeek-src
RUN mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DWOLFSSL_INCLUDE=/wolfssl-install/include \
        -DWOLFSSL_LIB=/wolfssl-install/lib/libwolfssl.so \
    && make -j$(nproc) tlspeek && \
    install -d /usr/local/include/tlspeek && \
    cp -f ../lib/*.h ../common/*.h /usr/local/include/tlspeek/ && \
    cp -f libtlspeek.a /usr/local/lib/libtlspeek.a && \
    ldconfig

# ── Stage 3: Build gateway with CGo ──────────────────────────────────────────
FROM --platform=$BUILDPLATFORM golang:1.24-bookworm AS build

ENV GO111MODULE=on
ENV CGO_ENABLED=1
ENV GOARCH=arm64
ENV GOOS=linux
ENV CC=aarch64-linux-gnu-gcc

ARG GIT_COMMIT
ARG VERSION

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Copy compiled wolfSSL and libtlspeek libraries.
COPY --from=wolfssl-build  /wolfssl-install  /usr/local
COPY --from=tlspeek-build  /usr/local/include/tlspeek /usr/local/include/tlspeek
COPY --from=tlspeek-build  /usr/local/lib/libtlspeek.a /usr/local/lib/

WORKDIR /go/src/github.com/openfaas/faas/gateway

COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/vendor         vendor
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/go.mod         go.mod
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/go.sum         go.sum

COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/handlers       handlers
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/metrics        metrics
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/requests       requests
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/types          types
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/plugin         plugin
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/version        version
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/scaling        scaling
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/pkg            pkg
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/httpmigrate    httpmigrate
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/main.go        .

# CGo flags: find wolfSSL + libtlspeek headers and static libs.
ENV CGO_CFLAGS="-I/usr/local/include -DHAVE_SECRET_CALLBACK -DWOLFSSL_KEYLOG_EXPORT"
ENV CGO_LDFLAGS="-L/usr/local/lib -lwolfssl -ltlspeek -lpthread -lm"

RUN go build --ldflags "-s -w \
    -X \"github.com/openfaas/faas/gateway/version.GitCommitSHA=${GIT_COMMIT}\" \
    -X \"github.com/openfaas/faas/gateway/version.Version=${VERSION}\"" \
    -o gateway .

# ── Stage 4: Runtime image ────────────────────────────────────────────────────
FROM debian:bookworm-slim AS ship

RUN apt-get update && apt-get install -y \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r app && useradd -r -g app app

# wolfSSL shared library (needed at runtime by the CGo gateway)
COPY --from=wolfssl-build /wolfssl-install/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

LABEL org.label-schema.license="OpenFaaS CE EULA - non-commercial" \
      org.label-schema.vcs-url="https://github.com/openfaas/faas" \
      org.label-schema.name="openfaas/faas-https" \
      org.label-schema.vendor="openfaas"

WORKDIR /home/app

EXPOSE 8080
EXPOSE 8082
EXPOSE 8443

ENV http_proxy=""
ENV https_proxy=""

COPY --from=build /go/src/github.com/openfaas/faas/gateway/gateway .
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/assets assets

RUN chown -R app:app ./

USER app

CMD ["./gateway"]
