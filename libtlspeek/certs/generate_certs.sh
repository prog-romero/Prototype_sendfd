#!/bin/bash
# generate_certs.sh — Generate self-signed TLS certificates for testing.
#
# Generates:
#   ca.key + ca.crt        — Self-signed Certificate Authority
#   server.key + server.csr — Server private key + CSR
#   server.crt             — Server certificate signed by the CA
#
# Usage:  cd libtlspeek && bash certs/generate_certs.sh

set -e

CERTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CERTS_DIR"

echo "=== Generating CA key and self-signed certificate ==="
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
    -subj "/C=FR/ST=Toulouse/L=Toulouse/O=ISAE-SUPAERO/CN=TestCA"

echo ""
echo "=== Generating server private key ==="
openssl genrsa -out server.key 2048

echo ""
echo "=== Generating server CSR ==="
openssl req -new -key server.key -out server.csr \
    -subj "/C=FR/ST=Toulouse/L=Toulouse/O=ISAE-SUPAERO/CN=localhost"

echo ""
echo "=== Signing server certificate with the CA ==="
cat > server_ext.cnf << 'EOF'
[v3_req]
subjectAltName = @alt_names
[alt_names]
DNS.1 = localhost
IP.1  = 127.0.0.1
EOF

openssl x509 -req -days 825 \
    -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt \
    -extfile server_ext.cnf \
    -extensions v3_req

rm -f server.csr server_ext.cnf ca.srl

echo ""
echo "=== Certificate summary ==="
openssl x509 -in server.crt -noout -subject -dates -subjectAltName

echo ""
echo "=== Done ==="
echo "Files generated in $CERTS_DIR:"
ls -lh ca.crt ca.key server.crt server.key
