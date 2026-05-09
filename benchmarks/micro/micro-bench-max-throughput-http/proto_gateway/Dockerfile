# syntax=docker/dockerfile:1

#
# bench2 initial-http gateway image (arm64).
#
# This image contains one custom OpenFaaS gateway binary with two optional
# benchmark listeners:
#   - vanilla HTTP forwarding benchmark on :8082
#   - prototype HTTP msg_peek + sendfd benchmark on :8083
#

FROM golang:1.24-bookworm AS build

ARG FAAS_COMMIT=f4dc39f8d87bf666fd5702791d752b382d90163b

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

COPY libtlspeek/common/*.h libtlspeek/common/*.c /src/benchhttp/
COPY benchmarks/micro/micro-bench-max-throughput-http/proto_gateway/benchhttp/ /src/benchhttp/
COPY benchmarks/micro/micro-bench-max-throughput-http/proto_gateway/patches/openfaas-faas-gateway-httpmigrate.patch /src/gateway-httpmigrate.patch

RUN install -d /out

RUN git clone https://github.com/openfaas/faas /src/faas \
 && cd /src/faas \
 && git checkout "${FAAS_COMMIT}" \
 && git apply --recount /src/gateway-httpmigrate.patch \
 && mkdir -p /src/faas/gateway/benchhttp \
 && cp -a /src/benchhttp/. /src/faas/gateway/benchhttp/

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
 && rm -rf /var/lib/apt/lists/*

WORKDIR /home/app

COPY --from=build /out/gateway /home/app/gateway
COPY --from=build /src/faas/gateway/assets /home/app/assets

RUN chmod 0755 /home/app/gateway

EXPOSE 8080
EXPOSE 8082
EXPOSE 8083

CMD ["./gateway"]
