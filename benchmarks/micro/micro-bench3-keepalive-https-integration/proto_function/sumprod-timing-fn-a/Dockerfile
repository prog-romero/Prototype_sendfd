# syntax=docker/dockerfile:1
# Dockerfile for timing-fn-a HTTPS (integration bench, keepalive HTTPS)
#
# Build context must be the repository root. Example:
#   docker build \
#     -f benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/timing-fn-a/Dockerfile \
#     -t timing-fn-a-ka-https:latest .

# ── Stage 1: Build wolfSSL from source ───────────────────────────────────────
FROM golang:1.24-bookworm AS wolfssl-build

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

# ── Stage 3: build fwatchdog (of-watchdog with sendfd relay) ─────────────────
FROM golang:latest AS watchdog-build
WORKDIR /go/src/github.com/openfaas/of-watchdog
COPY benchmarks/micro/micro-bench3-keepalive-https-integration/of-watchdog/ .
RUN rm -rf vendor && go mod download
RUN mkdir -p /out && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
    go build -ldflags "-s -w" -o /out/fwatchdog .

# ── Stage 4: build the HTTPS C worker ────────────────────────────────────────
FROM tlspeek-build AS fn-build

COPY --from=wolfssl-build  /wolfssl-install  /usr/local

COPY benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function/sumprod-timing-fn-a/timing_fn_ka_worker_https.c /src/worker.c
COPY libtlspeek/common/*.h libtlspeek/common/*.c /src/

RUN install -d /out && \
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
        -DHAVE_SECRET_CALLBACK -DWOLFSSL_KEYLOG_EXPORT \
        -I/src \
        -I/usr/local/include \
        -o /out/timing-fn-ka-worker \
        /src/worker.c \
        /src/sendfd.c \
        /src/unix_socket.c \
        -L/usr/local/lib \
        -lwolfssl -ltlspeek -lpthread -lm

# ── Stage 5: runtime image ────────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=wolfssl-build /wolfssl-install/lib/libwolfssl.so* /usr/local/lib/
RUN ldconfig

COPY --from=watchdog-build /out/fwatchdog           /usr/bin/fwatchdog
COPY --from=fn-build       /out/timing-fn-ka-worker /usr/local/bin/timing-fn-ka-worker

ENV fprocess="/usr/local/bin/timing-fn-ka-worker"
ENV mode=http
ENV sendfd_enable=1
ENV sendfd_socket_dir=/run/tlsmigrate

ENV HTTPMIGRATE_KA_FUNCTION_NAME=sumprod-timing-fn-a
ENV SENDFD_SOCKET_DIR=/run/tlsmigrate
ENV HTTPS_TLS_CERT=/certs/server.crt
ENV HTTPS_TLS_KEY=/certs/server.key

CMD ["/usr/bin/fwatchdog"]
