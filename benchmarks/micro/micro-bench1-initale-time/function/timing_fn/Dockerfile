FROM --platform=$BUILDPLATFORM golang:1.22-alpine AS build

WORKDIR /src
COPY go.mod ./
COPY go.sum ./
RUN go mod download
COPY main.go ./

ARG TARGETOS
ARG TARGETARCH
RUN CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH go build -trimpath -ldflags="-s -w" -o /out/timing_fn .

FROM alpine:3.20
COPY --from=build /out/timing_fn /timing_fn
EXPOSE 8080
ENTRYPOINT ["/timing_fn"]
