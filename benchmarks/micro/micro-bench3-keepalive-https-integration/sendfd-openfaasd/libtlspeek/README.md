# libtlspeek — TLS Read Peek Library

## Zero-copy FaaS Gateway with Stateless TLS Record Inspection

A research prototype implementing a novel FaaS (Function as a Service) gateway that **inspects HTTP headers inside encrypted TLS 1.3 records without consuming the kernel TCP buffer**, then transfers the live connection directly to the target worker process via `SCM_RIGHTS`.

### Novel Mechanism: `tls_read_peek()`

```
Client ──HTTPS──▶ Gateway                    Worker
                    │
                    ├─ recv(MSG_PEEK) ─▶ encrypted bytes (kernel buffer UNCHANGED)
                    ├─ Stateless AES-GCM decrypt ─▶ HTTP headers (plaintext)
                    ├─ Route: /function/hello ─▶ worker 0
                    ├─ tlspeek_serialize() ─▶ TLS session state struct
                    └─ sendmsg(SCM_RIGHTS) ──▶ fd + TLS state ──▶ Worker
                                                                     │
                                                                     ├─ wolfSSL_read()
                                                                     │   (same bytes!)
                                                                     └─ wolfSSL_write()
                                                                         (response direct)
```

The gateway **never touches the request body**. The worker decrypts the same
record from the kernel buffer (still intact because `MSG_PEEK` was used).

---

## Project Structure

```
libtlspeek/
├── CMakeLists.txt
├── lib/
│   ├── tlspeek.h           ← Public API + data structures
│   ├── tlspeek.c           ← tls_read_peek() + keylog callback
│   ├── tlspeek_crypto.h/c  ← Stateless AEAD + HKDF-Expand-Label
│   └── tlspeek_serial.h/c  ← Serialize / restore TLS session
├── gateway/
│   ├── gateway.c           ← Main gateway process (port 8443)
│   ├── router.h/c          ← URL-based routing decision
├── worker/
│   ├── worker.c            ← Worker process (Unix socket)
│   └── handler.c           ← HTTP response builder
├── common/
│   ├── sendfd.h/c          ← SCM_RIGHTS fd transfer
│   └── unix_socket.h/c     ← Unix domain socket helpers
├── certs/
│   └── generate_certs.sh   ← Self-signed cert generator
└── test/
    ├── test_tlspeek.c      ← Unit tests
    └── test_pipeline.sh    ← End-to-end curl test
```

---

## Prerequisites

### 1. Install wolfSSL

```bash
# From source (recommended — needs keylog + HKDF + AES-GCM)
git clone https://github.com/wolfSSL/wolfssl.git
cd wolfssl
./autogen.sh
./configure \
    --enable-tls13 \
    --enable-aesgcm \
    --enable-chacha \
    --enable-hkdf \
    --enable-opensslextra \
    --enable-keying-material \
    --enable-debug
make -j$(nproc)
sudo make install
sudo ldconfig
```

> **Key flags:**
> - `--enable-opensslextra` → enables `wolfSSL_SetClientWriteKey()` / `wolfSSL_SetServerWriteKey()`
> - `--enable-keying-material` → enables the keylog callback (`wolfSSL_CTX_set_keylog_callback`)
> - `--enable-hkdf` → enables `wc_HKDF_Expand()` for key derivation

### 2. Install CMake and build tools

```bash
sudo apt-get install -y cmake build-essential openssl curl
```

---

## Step 1 — Generate TLS Certificates

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek
bash certs/generate_certs.sh
```

This creates `certs/ca.crt`, `certs/server.crt`, and `certs/server.key`.

---

## Step 2 — Build the Project

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

Produces:
- `build/gateway` — the gateway binary
- `build/worker`  — the worker binary
- `build/test_tlspeek` — unit tests

---

## Step 3 — Run Unit Tests

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build
./test_tlspeek
```

Expected output: all tests `PASS`.

---

## Step 4 — Run the Full System (4 terminals)

> **Order matters:** workers must start before the gateway.

### Terminal 1 — Worker 0

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build
./worker 0 ../certs/server.crt ../certs/server.key
```

### Terminal 2 — Worker 1

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build
./worker 1 ../certs/server.crt ../certs/server.key
```

### Terminal 3 — Gateway

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek/build
./gateway 8443 ../certs/server.crt ../certs/server.key 2
```

### Terminal 4 — Test with curl

```bash
# Route to worker 0
curl -k https://localhost:8443/function/hello
# → Hello from worker 0!

# Route to worker 1
curl -k https://localhost:8443/function/compute
# → result=42

# POST echo (worker 0)
curl -k -X POST https://localhost:8443/function/echo -d "hello world"
# → hello world

# Status check (worker 1)
curl -k https://localhost:8443/function/status
# → {"worker":1,"status":"ok"}
```

Or run the automated end-to-end test:

```bash
cd /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/libtlspeek
bash test/test_pipeline.sh
```

---

## Routing Table

| Path                  | Worker | Method |
|-----------------------|--------|--------|
| `/function/hello`     | 0      | GET    |
| `/function/echo`      | 0      | POST   |
| `/function/compute`   | 1      | GET    |
| `/function/status`    | 1      | GET    |
| *(anything else)*     | 0      | any    |

---

## How It Works — Key Details

### Key Extraction (`tlspeek_keylog_cb`)

wolfSSL fires the keylog callback after the TLS 1.3 handshake, providing
`CLIENT_TRAFFIC_SECRET_0` and `SERVER_TRAFFIC_SECRET_0` in NSS Key Log format.
The callback derives the actual write keys and IVs using **HKDF-Expand-Label**
(RFC 8446 §7.1) via `wc_HKDF_Expand()`.

### Stateless Decryption (`tls_read_peek`)

```
nonce  = client_write_iv  XOR  seq_num_padded_to_12_bytes
result = AES-256-GCM-Decrypt(client_write_key, nonce, aad=TLS_header, ciphertext)
```

`seq_num` is **never incremented** — this is the stateless property.
The kernel buffer is **never consumed** — `MSG_PEEK` guarantees this.

### FD Transfer (`sendfd_with_state`)

A single `sendmsg()` call carries:
- **Ancillary data (SCM_RIGHTS):** the client file descriptor
- **Regular data (iovec):** the `tlspeek_serial_t` struct (keys + IVs + seq_nums)

After `sendmsg()` the gateway closes its copy of `client_fd`; the worker becomes
the sole owner.

---

## Constraints Satisfied

| Constraint | Status |
|---|---|
| wolfSSL only (no OpenSSL) | ✅ |
| TLS 1.3 only | ✅ |
| `MSG_PEEK` in `tls_read_peek()` | ✅ |
| `wc_AesGcmDecrypt()` for stateless decrypt | ✅ |
| `wolfSSL_CTX_set_keylog_callback()` | ✅ |
| `wc_HKDF_Expand()` for key derivation | ✅ |
| Single `sendmsg()` for fd + state | ✅ |
| `MSG_CTRUNC` check in `recvmsg()` | ✅ |
| Gateway closes fd after `sendmsg()` | ✅ |
| No new TLS handshake in worker | ✅ |
| Single-threaded gateway (`accept()` loop) | ✅ |
| No `wolfSSL_read()` in `tls_read_peek()` | ✅ |
| `read_seq_num` never advanced in peek | ✅ |
| Verbose `stderr` logging throughout | ✅ |

---

## Troubleshooting

### `tlspeek_restore` fails with "WOLFSSL_EXTRA not defined"

Recompile wolfSSL with `--enable-opensslextra`:
```bash
./configure --enable-tls13 --enable-aesgcm --enable-chacha \
            --enable-hkdf --enable-opensslextra --enable-keying-material
```

### keylog callback never fires (`keys_ready=0`)

Recompile wolfSSL with `--enable-keying-material` (or `WOLFSSL_HAVE_KEYING_MATERIAL`
define). Check that `wolfSSL_CTX_set_keylog_callback` is resolved at link time.

### `curl: (35) OpenSSL SSL_connect` errors

Use `curl -k` to skip certificate verification, or pass `--cacert certs/ca.crt`.

### Worker socket not found

Start the worker **before** the gateway. The gateway connects to
`/tmp/worker_N.sock` on startup.
