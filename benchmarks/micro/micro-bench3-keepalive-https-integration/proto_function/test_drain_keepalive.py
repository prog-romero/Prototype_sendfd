#!/usr/bin/env python3
"""
test_drain_keepalive.py — teste l'arrêt propre (503) du watchdog full-proxy sur
une connexion HTTPS keep-alive DÉJÀ migrée (le client parle en direct au watchdog,
le gateway est hors de la boucle).

Scénario :
  1. Ouvre 2 connexions TLS keep-alive et fait 1 requête sur chacune -> migration
     (à partir de là chaque connexion est servie DIRECTEMENT par le watchdog).
  2. Envoie un signal (SIGTERM par défaut) au watchdog dans le container, via SSH.
  3. Vérifie les deux comportements du drain :
       - conn IDLE   : on n'envoie RIEN. Le watchdog doit pousser un 503 non
                       sollicité en ~1 s (réveil de la lecture idle par SO_RCVTIMEO)
                       puis fermer.  <-- teste précisément notre correctif.
       - conn ACTIVE : on renvoie une requête sur la MÊME connexion. Doit répondre
                       503 Service Unavailable + Connection: close.

Avec --signal SIGKILL : aucun drain possible (signal non-interceptable), les deux
connexions doivent être coupées brutalement (reset / close) — c'est le résultat
ATTENDU, pas un bug.

Usage :
  python3 test_drain_keepalive.py
  python3 test_drain_keepalive.py --signal SIGKILL
  python3 test_drain_keepalive.py --host 192.168.2.2 --port 8443 \
      --fn sumprod-timing-fn-a --container sumprod-timing-fn-a \
      --pi-ssh romero@192.168.2.2
"""
import argparse
import socket
import ssl
import subprocess
import sys
import time


def build_request(host_hdr: str, fn: str, body: bytes = b"3 4", close: bool = False) -> bytes:
    conn = "close" if close else "keep-alive"
    head = (
        f"POST /function/{fn} HTTP/1.1\r\n"
        f"Host: {host_hdr}\r\n"
        f"Content-Type: text/plain\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: {conn}\r\n"
        f"\r\n"
    ).encode()
    return head + body


def recv_response(sock, timeout=6.0):
    """Retourne (status_line, headers_bytes, body_bytes, note). status_line=None si
    la connexion est fermée/reset avant toute réponse."""
    sock.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return None, b"", b"", "timeout (aucune réponse)"
        except (ConnectionResetError, OSError) as e:
            return None, buf, b"", f"reset/erreur ({e})"
        if not chunk:
            return None, buf, b"", "connexion fermée (EOF) sans réponse"
        buf += chunk

    header_part, _, rest = buf.partition(b"\r\n\r\n")
    status_line = header_part.split(b"\r\n", 1)[0].decode("latin1")
    cl = 0
    for line in header_part.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            try:
                cl = int(line.split(b":", 1)[1].strip())
            except ValueError:
                cl = 0
    body = rest
    while len(body) < cl:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        body += chunk
    return status_line, header_part, body, ""


def open_migrated_conn(host, port, host_hdr, fn):
    """Ouvre une connexion TLS keep-alive et fait la 1re requête (migration)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection((host, port), timeout=10)
    sock = ctx.wrap_socket(raw, server_hostname=host)
    sock.sendall(build_request(host_hdr, fn, body=b"3 4", close=False))
    status, _, body, note = recv_response(sock)
    return sock, status, body, note


def send_signal(pi_ssh, container, sig):
    cmd = ["ssh", pi_ssh, "sudo", "ctr", "-n", "openfaas-fn",
           "task", "kill", "-s", sig, container]
    print(f"    $ {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description="Test drain 503 sur connexion keep-alive migrée.")
    ap.add_argument("--host", default="192.168.2.2")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--fn", default="sumprod-timing-fn-a", help="nom de la fonction (chemin /function/<fn>)")
    ap.add_argument("--container", default="sumprod-timing-fn-a", help="nom du container containerd (ctr task)")
    ap.add_argument("--pi-ssh", default="romero@192.168.2.2")
    ap.add_argument("--signal", default="SIGTERM", help="SIGTERM (défaut) ou SIGKILL")
    ap.add_argument("--wait", type=float, default=2.0, help="pause après le signal avant de tester (s)")
    args = ap.parse_args()

    host_hdr = f"{args.host}:{args.port}"

    print(f"=== Test drain keep-alive [{args.signal}] sur {args.fn} @ {host_hdr} ===\n")

    # 1. Deux connexions migrées.
    print("[1] Ouverture de 2 connexions HTTPS keep-alive + 1re requête (migration)…")
    try:
        conn_idle, s1, b1, n1 = open_migrated_conn(args.host, args.port, host_hdr, args.fn)
        conn_active, s2, b2, n2 = open_migrated_conn(args.host, args.port, host_hdr, args.fn)
    except Exception as e:
        print(f"    ÉCHEC ouverture/migration : {e}")
        print("    (la fonction est-elle déployée et le gateway up sur 8443 ?)")
        sys.exit(1)
    print(f"    conn IDLE   : {s1}   body={b1!r} {('['+n1+']') if n1 else ''}")
    print(f"    conn ACTIVE : {s2}   body={b2!r} {('['+n2+']') if n2 else ''}")
    if not (s1 and s1.endswith('200 OK')) or not (s2 and s2.endswith('200 OK')):
        print("    ⚠️  la 1re requête n'a pas renvoyé 200 OK — migration KO, on arrête.")
        sys.exit(1)

    # 2. Signal au watchdog (PID 1 du container = fwatchdog).
    print(f"\n[2] Envoi de {args.signal} au watchdog du container '{args.container}'…")
    try:
        send_signal(args.pi_ssh, args.container, args.signal)
    except subprocess.CalledProcessError as e:
        print(f"    ÉCHEC envoi signal : {e}")
        sys.exit(1)

    print(f"\n[3] Attente {args.wait:.0f}s puis vérification…")
    time.sleep(args.wait)

    expect_503 = args.signal.upper() in ("SIGTERM", "TERM", "15", "SIGINT", "INT", "2")

    # 3a. conn IDLE : on ne renvoie RIEN, on lit -> 503 non sollicité attendu (SIGTERM).
    print("\n[3a] conn IDLE (on n'envoie rien, on lit ce que pousse le watchdog) :")
    si, hi, bi, ni = recv_response(conn_idle, timeout=6.0)
    if si:
        print(f"     reçu : {si}   body={bi!r}")
        ok_idle = expect_503 and "503" in si
        print(f"     -> {'✅ PASS' if ok_idle else ('❌ FAIL' if expect_503 else 'ℹ️')} "
              f"(attendu : {'503 non sollicité' if expect_503 else 'reset (SIGKILL)'})")
    else:
        print(f"     pas de réponse : {ni}")
        print(f"     -> {'✅ PASS (reset attendu)' if not expect_503 else '❌ FAIL (on attendait un 503)'}")

    # 3b. conn ACTIVE : on renvoie une requête sur la MÊME connexion -> 503 attendu.
    print("\n[3b] conn ACTIVE (2e requête sur la MÊME connexion keep-alive) :")
    try:
        conn_active.sendall(build_request(host_hdr, args.fn, body=b"5 6", close=False))
        sa, ha, ba, na = recv_response(conn_active, timeout=6.0)
        if sa:
            print(f"     reçu : {sa}   body={ba!r}")
            has_close = ha is not None and b"connection: close" in ha.lower()
            ok_active = expect_503 and "503" in sa
            print(f"     Connection: close présent : {has_close}")
            print(f"     -> {'✅ PASS' if ok_active else ('❌ FAIL' if expect_503 else 'ℹ️')} "
                  f"(attendu : {'503 + close' if expect_503 else 'reset (SIGKILL)'})")
        else:
            print(f"     pas de réponse : {na}")
            print(f"     -> {'✅ PASS (reset attendu)' if not expect_503 else '❌ FAIL (on attendait un 503)'}")
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        print(f"     write/read a échoué : {e}")
        print(f"     -> {'✅ PASS (reset attendu)' if not expect_503 else '❌ FAIL (on attendait un 503)'}")

    for s in (conn_idle, conn_active):
        try:
            s.close()
        except OSError:
            pass

    print("\n=== fin ===")
    if expect_503:
        print("Note : après le drain le container est STOPPED. Une requête via le gateway")
        print("       (curl https://.../function/…) le redémarre (scale-from-zero).")


if __name__ == "__main__":
    main()
