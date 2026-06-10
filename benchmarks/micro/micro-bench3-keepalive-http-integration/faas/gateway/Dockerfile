FROM golang:1.24-bookworm AS build

ENV GO111MODULE=on
ENV CGO_ENABLED=0
ENV GOARCH=arm64
ENV GOOS=linux

ARG GIT_COMMIT
ARG VERSION

WORKDIR /go/src/github.com/openfaas/faas/gateway

COPY vendor         vendor
COPY go.mod         go.mod
COPY go.sum         go.sum

COPY handlers       handlers
COPY metrics        metrics
COPY requests       requests

COPY types          types
COPY plugin         plugin
COPY version        version
COPY scaling        scaling
COPY pkg            pkg
COPY httpmigrate    httpmigrate
COPY main.go        .

# RUN license-check -path ./ --verbose=false "Alex Ellis" "OpenFaaS Authors" "OpenFaaS Author(s)" || true

# Run a gofmt and exclude all vendored code.
# RUN test -z "$(gofmt -l $(find . -type f -name '*.go' -not -path "./vendor/*"))"
# RUN go test $(go list ./... | grep -v integration | grep -v /vendor/ | grep -v /template/) -cover

RUN go build --ldflags "-s -w \
    -X \"github.com/openfaas/faas/gateway/version.GitCommitSHA=${GIT_COMMIT}\" \
    -X \"github.com/openfaas/faas/gateway/version.Version=${VERSION}\"" \
    -o gateway .

FROM alpine:3.21 AS ship

LABEL org.label-schema.license="OpenFaaS CE EULA - non-commercial" \
    org.label-schema.vcs-url="https://github.com/openfaas/faas" \
    org.label-schema.vcs-type="Git" \
    org.label-schema.name="openfaas/faas" \
    org.label-schema.vendor="openfaas" \
    org.label-schema.docker.schema-version="1.0"

LABEL org.opencontainers.image.source=https://github.com/openfaas/faas

RUN addgroup -S app \
    && adduser -S -g app app \
    && apk add --no-cache ca-certificates

WORKDIR /home/app

EXPOSE 8080
EXPOSE 8082
ENV http_proxy=""
ENV https_proxy=""

COPY --from=build /go/src/github.com/openfaas/faas/gateway/gateway    .
COPY assets     assets

ARG TARGETPLATFORM

RUN if [ "$TARGETPLATFORM" = "linux/arm/v7" ] ; then sed -ie s/x86_64/armhf/g assets/script/funcstore.js ; elif [ "$TARGETPLATFORM" = "linux/arm64" ] ; then sed -ie s/x86_64/arm64/g assets/script/funcstore.js; fi

RUN chown -R app:app ./

USER app

CMD ["./gateway"]

