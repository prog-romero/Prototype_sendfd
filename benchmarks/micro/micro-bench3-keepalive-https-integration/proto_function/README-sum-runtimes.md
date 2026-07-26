# Fonction `sum` en Python et JS (vanilla + prototype)

Même charge triviale que la fonction C `sumprod-vanilla-fn-a` (**somme de deux
nombres**), portée sur deux autres runtimes pour comparer le **CPU par composant**
du workflow vanilla selon le langage du container :

| Dossier | Runtime | Style | Serveur métier (127.0.0.1:8085) |
|---|---|---|---|
| `sum-python/` | Python | SeBS (cf. graph-pagerank) | Flask + gunicorn (`handler(event)`) |
| `sum-js/` | Node.js | BeFaaS (node-express) | Express (`server.js`) |

Chaque runtime a **2 images** (même serveur métier, watchdog différent) :

| Image | Watchdog | sendfd |
|---|---|---|
| `romerosdd/sum-python-vanilla` | of-watchdog CGO=0 | non |
| `romerosdd/sum-python-fullproxy` | of-watchdog + wolfSSL | oui |
| `romerosdd/sum-js-vanilla` | of-watchdog CGO=0 | non |
| `romerosdd/sum-js-fullproxy` | of-watchdog + wolfSSL | oui |

**Entrée acceptée** (compat client d'éval) : JSON `{"a":3,"b":4}` **ou** texte
`"3 4"` (donc les mêmes fichiers que la fonction C : `sebs-bench/inputs/sumprod.txt`,
`sumprod_500kb.txt`). Réponse : `{"status":"success","result":7,...}`.

---

## 1. Build + push (machine dev, depuis la RACINE du dépôt)

Contexte de build = racine du dépôt (les Dockerfiles copient `of-watchdog/`,
`wolfssl/`, `libtlspeek/`). arm64 pour le Pi.

```bash
cd <repo-root>
B=benchmarks/micro/micro-bench3-keepalive-https-integration/proto_function

# --- Python ---
docker buildx build --platform linux/arm64 -f $B/sum-python/Dockerfile.vanilla \
  -t romerosdd/sum-python-vanilla:latest   --push .
docker buildx build --platform linux/arm64 -f $B/sum-python/Dockerfile.fullproxy \
  -t romerosdd/sum-python-fullproxy:latest --push .

# --- JS ---
docker buildx build --platform linux/arm64 -f $B/sum-js/Dockerfile.vanilla \
  -t romerosdd/sum-js-vanilla:latest   --push .
docker buildx build --platform linux/arm64 -f $B/sum-js/Dockerfile.fullproxy \
  -t romerosdd/sum-js-fullproxy:latest --push .
```
Variables : `REGISTRY`/tags à adapter si autre registre. `--push` → Docker Hub ;
`--load` pour rester local.

---

## 2. Déploiement (sur le Pi)

Les YAML sont dans `../deploy/`. Le **gateway** doit être dans le bon mode :
- **vanilla** : `HTTPMIGRATE_ENABLE=0` (+ `HTTPS_ENABLE=1` pour le 8443).
- **proto**   : `HTTPMIGRATE_ENABLE=1` (+ `HTTPS_ENABLE=1`).

```bash
cd ~/…/micro-bench3-keepalive-https-integration/deploy

# secrets TLS (une fois, requis par les images fullproxy) :
#   faas-cli secret create server-crt --from-file=server.crt
#   faas-cli secret create server-key --from-file=server.key

# force le re-pull (:latest est caché par faasd)
for img in sum-python-vanilla sum-python-fullproxy sum-js-vanilla sum-js-fullproxy; do
  sudo ctr -n openfaas-fn image rm docker.io/romerosdd/$img:latest 2>/dev/null || true
done

# --- passe VANILLA (gateway HTTPMIGRATE_ENABLE=0) ---
faas-cli deploy -f sum-python-vanilla.yml
faas-cli deploy -f sum-js-vanilla.yml

# --- passe PROTO (gateway HTTPMIGRATE_ENABLE=1) ---
faas-cli deploy -f sum-python-proto.yml
faas-cli deploy -f sum-js-proto.yml
```

**Smoke test** (vanilla → 8443 terminé ; proto → 8443 migré) :
```bash
curl -sk -X POST https://192.168.2.2:8443/function/sum-python-vanilla -d '3 4'   # -> result 7
curl -sk -X POST https://192.168.2.2:8443/function/sum-js-vanilla      -d '3 4'   # -> result 7
```

---

## 3. Éval CPU par composant

Réutilise `sebs-bench/eval/cpu_composant/run_sweep_cpu.py` en changeant juste
`--function` (le résolveur pidstat capte automatiquement le process métier enfant
du fwatchdog : **gunicorn** pour Python, **node** pour JS) :

```bash
cd ~/…/sebs-bench/eval/cpu_composant
python3 run_sweep_cpu.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function sum-python-vanilla --input ../../inputs/sumprod.txt \
  --rates 128,256,512,768,1024 --conn-mode keepalive --concurrency 56 --threads 8 \
  --duration-s 60 --pi-ssh romero@192.168.2.2 \
  --out results/vanilla_sum_python.csv --pidstat-dir results/pidstat_sum_python
# idem avec --function sum-js-vanilla, --out …_sum_js.csv
```
Tu obtiens alors le camembert (gateway / faasd-provider / containerd / autres)
par **runtime** → comparaison C vs Python vs JS du coût container.

---

## Notes
- Serveur métier **identique** entre vanilla et proto d'un même runtime : seul le
  watchdog (et son env sendfd) change → la comparaison isole l'effet de la migration.
- Python : `GUNICORN_WORKERS=4` (multi-cœur) — ajustable dans le YAML.
- JS : Express lit tout corps en texte puis tente JSON (limite 32 Mo, ok pour 500 Ko).
- Le fullproxy JS installe Node via tarball sur `debian:trixie-slim` (glibc requise
  par le watchdog CGO) ; le fullproxy Python installe python3 sur la même base.
