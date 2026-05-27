# --- Stage 1: Import the official OpenFaaS Watchdog ---
FROM ghcr.io/openfaas/classic-watchdog:0.3.9 AS watchdog

# --- Stage 2: Compile your fixed, unbuffered C code ---
FROM alpine:3.18 AS builder
RUN apk add --no-cache build-base
WORKDIR /src
COPY main.c .
RUN gcc -static main.c -o sum_worker

# --- Stage 3: Build the final clean execution container ---
FROM alpine:3.18
RUN addgroup -S app && adduser -S app -G app
WORKDIR /home/app

# EXTRACT THE FWATCHDOG BINARY DIRECTLY FROM STAGE 1
COPY --from=watchdog /fwatchdog /usr/bin/fwatchdog

# Extract your compiled C binary from Stage 2
COPY --from=builder /src/sum_worker .

# Fix the execution permissions manually
USER root
RUN chmod +x /usr/bin/fwatchdog && chmod +x /home/app/sum_worker
RUN chown -R app:app /home/app

USER app
ENV fprocess="/home/app/sum_worker"
EXPOSE 8080
CMD ["/usr/bin/fwatchdog"]
