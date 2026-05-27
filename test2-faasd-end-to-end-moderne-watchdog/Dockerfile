# --- Stage 1: Pull the Modern OpenFaaS HTTP Watchdog ---
FROM ghcr.io/openfaas/of-watchdog:0.9.15 AS watchdog

# --- Stage 2: Compile the persistent C webserver ---
FROM alpine:3.18 AS builder
RUN apk add --no-cache build-base
WORKDIR /src
COPY main.c .
RUN gcc -static main.c -o sum_server

# --- Stage 3: Assemble the modern image ---
FROM alpine:3.18
RUN addgroup -S app && adduser -S app -G app
WORKDIR /home/app

# Import the modern watchdog binary
COPY --from=watchdog /fwatchdog /usr/bin/fwatchdog

# Import your compiled micro-server binary
COPY --from=builder /src/sum_server .

USER root
RUN chmod +x /usr/bin/fwatchdog && chmod +x /home/app/sum_server
RUN chown -R app:app /home/app

USER app

# CONFIGURE THE MODERN MODE ENVIRONMENT FLAGS
ENV mode="http"
ENV upstream_url="http://127.0.0.1:5000"
ENV fprocess="/home/app/sum_server"

EXPOSE 8080
CMD ["/usr/bin/fwatchdog"]
