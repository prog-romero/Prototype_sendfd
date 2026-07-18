#!/usr/bin/env python3
"""
test_keepalive_switch.py — teste la migration de connexion en cas de "wrong-owner"
keep-alive.

Ouvre UNE SEULE connexion TLS (keep-alive) vers le gateway et envoie N requêtes
en ALTERNANT la fonction cible à chaque requête (dynamic-html / graph-pagerank).
C'est le cas le plus dur du prototype : une même connexion cliente dont la
requête suivante vise une AUTRE fonction que celle qui la détient → le watchdog
détecte le mauvais propriétaire (TLSPeek) et ré-émet l'objet de migration vers le
provider, qui re-route vers le bon conteneur.

Le test vérifie que :
  - toutes les requêtes réussissent (200) ;
  - elles passent bien sur LE MÊME socket (même port local du début à la fin)
    → preuve que la connexion cliente n'a pas été coupée/renégociée.

Usage :
  python3 test_keepalive_switch.py --host 192.168.2.2 --scheme https --count 5
  python3 test_keepalive_switch.py --host 192.168.2.2 --scheme http  --count 6
"""
from __future__ import annotations

import argparse
import http.client
import json
import ssl
import sys

# Payloads (identiques aux ../inputs/*.json)
PAYLOADS = {
    "dynamic-html": {"username": "testname", "random_len": 1000},
    "graph-pagerank": {"size": 510, "seed": 42},
}


def main() -> int:
    p = argparse.ArgumentParser(description="Test keep-alive multi-fonctions (wrong-owner migration).")
    p.add_argument("--host", default="192.168.2.2")
    p.add_argument("--scheme", choices=["https", "http"], default="https")
    p.add_argument("--port", type=int, default=None, help="override (def 8443/8080)")
    p.add_argument("--count", type=int, default=5, help="nombre de requêtes (def 5)")
    p.add_argument("--functions", default="dynamic-html,graph-pagerank",
                   help="fonctions à alterner (séparées par des virgules)")
    p.add_argument("--timeout", type=float, default=30.0)
    args = p.parse_args()

    port = args.port or (8443 if args.scheme == "https" else 8080)
    fns = [f.strip() for f in args.functions.split(",") if f.strip()]
    if not fns:
        print("ERREUR: aucune fonction", file=sys.stderr)
        return 2

    # UNE connexion, keep-alive. Cert auto-signé -> pas de vérification en HTTPS.
    if args.scheme == "https":
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        conn = http.client.HTTPSConnection(args.host, port, timeout=args.timeout, context=ctx)
    else:
        conn = http.client.HTTPConnection(args.host, port, timeout=args.timeout)

    print(f"=== keep-alive multi-fonctions : {args.scheme}://{args.host}:{port} ===")
    print(f"    {args.count} requêtes, alternance : {fns}\n")

    conn.connect()  # établit le socket (et le handshake TLS) UNE fois
    local_ports = set()
    ok = 0

    for i in range(args.count):
        fn = fns[i % len(fns)]
        body = json.dumps(PAYLOADS.get(fn, {})).encode()
        try:
            conn.request("POST", f"/function/{fn}", body=body,
                         headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            data = resp.read()  # DOIT tout lire avant la requête suivante (keep-alive)
        except Exception as exc:  # noqa: BLE001
            print(f"  #{i + 1:<2} {fn:<15} -> ÉCHEC : {exc}")
            print("\n[FAIL] la connexion a été rompue -> la migration wrong-owner "
                  "ne fonctionne pas (ou fonction absente).")
            conn.close()
            return 1

        # Port local du socket client : DOIT rester identique (même connexion).
        try:
            lport = conn.sock.getsockname()[1]
        except Exception:  # noqa: BLE001
            lport = -1
        local_ports.add(lport)

        snippet = data[:70].decode(errors="replace").replace("\n", " ")
        flag = "OK " if resp.status == 200 else "!! "
        print(f"  #{i + 1:<2} {fn:<15} -> {flag}HTTP {resp.status}  "
              f"local_port={lport}  {len(data):>6}B  | {snippet}")
        if resp.status == 200:
            ok += 1

    conn.close()

    print()
    same_conn = len(local_ports) == 1 and -1 not in local_ports
    print(f"  résultat : {ok}/{args.count} en 200 · "
          f"{'MÊME connexion' if same_conn else 'connexions DIFFÉRENTES'} "
          f"(ports locaux vus : {sorted(local_ports)})")
    if ok == args.count and same_conn:
        print("\n[OK] migration wrong-owner validée : toutes les fonctions ont répondu "
              "sur une SEULE connexion keep-alive.")
        return 0
    print("\n[FAIL] anomalie : voir les codes/ports ci-dessus.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
