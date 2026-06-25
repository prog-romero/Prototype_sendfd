# Macro-bench BeFaaS IoT sur faasd — vanilla vs prototype (sendfd)

Macro-évaluation avec une **vraie application** (BeFaaS IoT, fonctions **non
modifiées**) déployée sur faasd, pour comparer le chemin **vanilla** (HTTP/HTTPS
standard) au **prototype** (migration `sendfd` + état TLS, full-proxy watchdog).

On fait **deux évaluations séparées** :
- une **tout HTTP**  (le client et tous les appels inter-fonctions sont en HTTP) ;
- une **tout HTTPS** (le client et tous les appels inter-fonctions sont en HTTPS).

---

## 1. Les 4 fonctions et le graphe d'appels

Ce sont les fonctions **exactes** du dépôt `BeFaaS-framework` (`experiments/iot`).
Seule modification : la ligne `ctx.lib.call('publisher', …)` d'`objectrecognition`
est commentée (canal pub/sub asynchrone impossible sur OpenFaaS pur, hors du
chemin synchrone qu'on mesure).

```
client ──► objectrecognition ──ctx.call──► emergencydetection ──ctx.call──► setlightphasecalculation
                  │                                                            (Redis : état des feux)
                  └────────────ctx.call──► trafficstatistics  (Redis : stats)
```

| Fonction | Type BeFaaS | Route | Appelle | État |
|---|---|---|---|---|
| `objectrecognition` | `router` | `POST /` | trafficstatistics, emergencydetection | — |
| `emergencydetection` | `rpcHandler` | `POST /call` | setlightphasecalculation | — |
| `trafficstatistics` | `rpcHandler` | `POST /call` | — | **Redis** |
| `setlightphasecalculation` | `rpcHandler` | `POST /call` | — | **Redis** |

Profondeur max de la chaîne : **3 hops** (objectrecognition → emergencydetection
→ setlightphasecalculation) → chaque hop est migré en mode prototype.

---

## 2. Ce qu'il y a DANS chaque conteneur

Identique aux fonctions `sumprod`, sauf que le binaire métier est remplacé par un
**serveur Node** (template `node10-express-service` + fonction BeFaaS) :

```
┌──────────── conteneur "<fonction>" ────────────────────────────┐
│ fwatchdog (PID 1)                                              │
│   • vanilla  : reverse-proxy HTTP simple (sendfd_enable=0)     │
│   • proto    : full-proxy (peek TLS, route, relai, keep-alive, │
│                read/write wolfSSL) puis reverse-proxy          │
│                          │ http://127.0.0.1:8085               │
│                          ▼                                     │
│ node index.js  (template/index.js = node10-express-service)   │
│   └─ function/handler.js  → branche le openfaasHandler BeFaaS  │
│        + normalise le chemin (/function/<nom>/call → /call)    │
│        └─ function/index.js = LA fonction BeFaaS (inchangée)   │
│             require('@befaas/lib') → Koa + ctx.call + ctx.db   │
└────────────────────────────────────────────────────────────────┘
```

- `template/` = l'hôte (le template OpenFaaS), **partagé** par les 4 fonctions.
  Le **seul** ajout par rapport à BeFaaS est la normalisation du chemin dans
  `handler.js` (le gateway de migration rejoue l'URL brute `/function/<nom>/…`,
  alors que le routeur Koa de BeFaaS attend `/call`). Le watchdog n'est PAS modifié.
- `functions/<nom>/index.js` = le code métier BeFaaS, non modifié.
- `ctx.call(f)` → `POST $OPENFAAS_ENDPOINT/$f/call` → repasse par le gateway →
  migré (proto) ou re-proxifié (vanilla).
- `ctx.db` → Redis (`REDIS_ENDPOINT`).

---

## 3. Pré-requis

- Le Pi tourne faasd, avec le **gateway prototype** (migration) sur `:8443`
  (HTTPS) et `:8080` (HTTP), exactement comme pour `sumprod`.
- Les secrets `server-crt` / `server-key` existent déjà (`faas-cli secret ls`).
- `docker buildx` configuré pour `linux/arm64` sur la machine de build.
- `faas-cli` configuré et connecté au gateway sur le Pi.

---

## 4. Étapes — DANS L'ORDRE

> Toutes les commandes `docker buildx` se lancent depuis la **racine du dépôt**
> (le contexte de build = racine, car les Dockerfiles référencent `wolfssl/`,
> `libtlspeek/`, `of-watchdog/`, etc.).

### Étape 0 — Déployer Redis (une fois)

`trafficstatistics` et `setlightphasecalculation` ont besoin de Redis.

```bash
# Sur le Pi :
sudo nano /var/lib/faasd/docker-compose.yaml
#   → coller le service `redis:` de deploy/redis-compose-snippet.yml
#     dans la section services: (IP statique 10.62.0.5)
sudo systemctl restart faasd

# Vérifier :
redis-cli -h 10.62.0.5 ping        # → PONG
```

### Étape 1 — Construire et pousser les images

```bash
cd <repo-root>

# Vanilla (4 images) :
./benchmarks/micro/micro-bench3-keepalive-https-integration/macro-befaas-iot/build-all.sh vanilla

# Prototype full-proxy (4 images) — réutilise le cache wolfSSL/tlspeek :
./benchmarks/micro/micro-bench3-keepalive-https-integration/macro-befaas-iot/build-all.sh proto

# (ou les 8 d'un coup : ... build-all.sh all)
```

Images produites (registre `romerosdd` par défaut, modifiable via `REGISTRY=`) :
```
romerosdd/iot-vanilla-{objectrecognition,emergencydetection,trafficstatistics,setlightphasecalculation}:latest
romerosdd/iot-fullproxy-{…}:latest
```

> Le 1er build `proto` compile wolfSSL (~13 min sous émulation arm64). Les builds
> suivants réutilisent le cache. Node est installé via le tarball officiel arm64
> sur `debian:trixie-slim` (compat GLIBC du watchdog CGO).

### Étape 2 — Déployer (sur le Pi)

`deploy-all.sh <vanilla|proto> <http|https>` réécrit `OPENFAAS_ENDPOINT` et
`REDIS_ENDPOINT` puis déploie les 4 fonctions.

```bash
cd .../macro-befaas-iot/deploy

# --- Évaluation TOUT HTTP ---
./deploy-all.sh vanilla http      # baseline HTTP
# (mesurer)  … puis :
./deploy-all.sh proto   http      # prototype HTTP

# --- Évaluation TOUT HTTPS ---
./deploy-all.sh vanilla https     # baseline HTTPS (TLS terminé au gateway)
# (mesurer)  … puis :
./deploy-all.sh proto   https     # prototype HTTPS (migration de l'état TLS)
```

Variables utiles : `GATEWAY_IP=` (def 192.168.2.2), `REDIS_IP=` (def 10.62.0.5).

> vanilla et proto déploient les **mêmes noms de fonctions** : redéployer un KIND
> remplace l'autre. Faites donc : déployer vanilla → mesurer → déployer proto →
> mesurer, pour chaque MODE.

### Étape 3 — Vérifier le chemin de bout en bout

```bash'
faas-cli list --gateway http://127.0.0.1:8080      # les 4 fonctions Ready

# Test direct d'une fonction rpc (emergencydetection) :
curl -s http://127.0.0.1:8080/function/emergencydetection/call \
  -H 'Content-Type: application/json' \
  -d '{"objects":[{"type":"ambulance"}]}' | jq .
# → {"emergency":{"active":true,"type":"ambulance"}}  (+ appel setlightphasecalculation)

# Point d'entrée objectrecognition (multipart image) :
#   image ROUGE (pixel 0,0 .r>0) => déclenche l'ambulance/emergency
curl -s http://127.0.0.1:8080/function/objectrecognition \
  -F "image=@/path/to/red.png" | jq .
```

Pour l'éval HTTPS, taper le gateway sur `:8443` en `https://` (avec `-k` pour le
cert auto-signé).

---

## 5. La matrice d'évaluation

| Éval | KIND | Client → gateway | OPENFAAS_ENDPOINT (inter-fonctions) |
|---|---|---|---|
| HTTP baseline | vanilla | `http://…:8080` | `http://…:8080/function` |
| HTTP proto | proto | `http://…:8080` | `http://…:8080/function` |
| HTTPS baseline | vanilla | `https://…:8443` (TLS au gateway) | `https://…:8443/function` |
| HTTPS proto | proto | `https://…:8443` (TLS migré) | `https://…:8443/function` |

La charge se génère sur `objectrecognition` (point d'entrée) ; chaque requête
déclenche la chaîne complète à 3 hops. (Générateur de charge : étape suivante —
on pourra réutiliser wrk2/artillery ; l'upload multipart d'`objectrecognition`
impose artillery ou un script multipart.)

---

## 6. Points d'attention (comportement RÉEL des fonctions BeFaaS)

- **`setlightphasecalculation`** contient un `setTimeout` délibéré
  (`waitAppropriately`, ≥ 2 s) **et** un verrou Redis (`lightcalculation:lock`).
  Sous charge, la plupart des appels retournent `{}` immédiatement (verrou tenu),
  mais le détenteur dort ≥ 2 s. C'est le comportement BeFaaS d'origine. Si cette
  latence domine vos mesures, mesurez aussi la branche
  `objectrecognition → trafficstatistics` séparément.
- **`objectrecognition`** lit une **image** (multipart + `jimp`). Le coût `jimp`
  s'ajoute identiquement en vanilla et en proto (donc la *différence* reste
  significative), mais ajoute de la variance sur Pi.
- **Redis** est partagé par `trafficstatistics` et `setlightphasecalculation` ;
  le coût Redis est identique dans les deux modes.

---

## 7. Fidélité au bench BeFaaS

- `functions/*/index.js` = sources BeFaaS **exactes** (sauf 1 ligne publisher
  commentée dans objectrecognition).
- `@befaas/lib`, `lodash`, `jimp`, `@koa/multer`, `multer` installés via npm
  (mêmes versions que `experiments/iot/package.json` de BeFaaS).
- `experiment.json` minimal (4 fonctions, provider `openfaas`) lu par
  `@befaas/lib` pour résoudre `ctx.call`.
- L'hôte = template OpenFaaS `node10-express-service` officiel, + normalisation
  de chemin (nécessaire car le gateway de migration rejoue l'URL brute).
```
