#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "${SCRIPT_DIR}/build_copy_enable_proto_gateway.sh"

    environment:
      basic_auth: "true"
      functions_provider_url: "http://10.62.0.1:8081/"
      direct_functions: "false"
      read_timeout: "60s"
      write_timeout: "60s"
      upstream_timeout: "65s"
      faas_nats_address: "nats"
      faas_nats_port: "4222"
      function_namespace: "openfaas-fn"
      HTTPMIGRATE_ENABLE: "1"
      HTTPMIGRATE_LISTEN: ":8083"
      HTTPMIGRATE_SOCKET_DIR: "/run/secrets/httpmigrate"
    volumes:
      - source: ./secrets/basic-auth-password
        target: /run/secrets/basic-auth-password
        type: bind
      - source: ./secrets/basic-auth-user
        target: /run/secrets/basic-auth-user
        type: bind
      - source: /var/lib/faasd/secrets/httpmigrate
        target: /run/secrets/httpmigrate
        type: bind
