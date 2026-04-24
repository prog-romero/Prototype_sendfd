#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 OUTPUT_JSON" >&2
    exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
OUT_JSON="$1"

LISTEN_ADDR="${LISTEN_ADDR:-127.0.0.1:9446}"
BENCH_PATH="${BENCH_PATH:-/bench}"
CERT_PATH="${CERT_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.crt}"
KEY_PATH="${KEY_PATH:-${ROOT_DIR}/../../../libtlspeek/certs/server.key}"
TLS_VERSION="${TLS_VERSION:-1.2}"

case "${TLS_VERSION}" in
  1.2)
    CADDY_TLS_MIN="tls1.2"
    CADDY_TLS_MAX="tls1.2"
    ;;
  1.3)
    CADDY_TLS_MIN="tls1.3"
    CADDY_TLS_MAX="tls1.3"
    ;;
  *)
    echo "ERROR: TLS_VERSION must be 1.2 or 1.3" >&2
    exit 1
    ;;
esac

mkdir -p "$(dirname -- "${OUT_JSON}")"

cat > "${OUT_JSON}" <<EOF
{
  "admin": {
    "disabled": true
  },
  "apps": {
    "http": {
      "servers": {
        "bench": {
          "listen": [
            "${LISTEN_ADDR}"
          ],
          "listener_wrappers": [
            {
              "wrapper": "bench_top1"
            }
          ],
          "automatic_https": {
            "disable": true
          },
          "protocols": [
            "h1"
          ],
          "routes": [
            {
              "match": [
                {
                  "path": [
                    "${BENCH_PATH}"
                  ]
                }
              ],
              "handle": [
                {
                  "handler": "bench_top2"
                }
              ],
              "terminal": true
            }
          ],
          "tls_connection_policies": [
            {
              "certificate_selection": {
                "any_tag": [
                  "bench-cert"
                ]
              },
              "alpn": [
                "http/1.1"
              ],
              "protocol_min": "${CADDY_TLS_MIN}",
              "protocol_max": "${CADDY_TLS_MAX}"
            }
          ]
        }
      }
    },
    "tls": {
      "certificates": {
        "load_files": [
          {
            "certificate": "${CERT_PATH}",
            "key": "${KEY_PATH}",
            "tags": [
              "bench-cert"
            ]
          }
        ]
      }
    }
  }
}
EOF

echo "Wrote ${OUT_JSON}"