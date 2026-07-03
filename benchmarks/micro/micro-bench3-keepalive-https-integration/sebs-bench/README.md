# sebs-bench — SeBS (référence) adapté à faasd : Vanilla vs Prototype (sendfd)

On évalue le prototype (migration `sendfd` + état TLS, watchdog full-proxy) face
au chemin **vanilla** (proxy HTTP/HTTPS standard) en utilisant les **applications
du benchmark de référence [SeBS](https://github.com/spcl/serverless-benchmarks)**
(SPCL/ETH Zürich, Middleware 2021).

## Le principe (important)

SeBS ne supporte **ni OpenFaaS ni faasd** : son driver cible AWS/Azure/GCP/
OpenWhisk. On **n'utilise donc PAS le driver SeBS**. À la place, on reprend les
**3 briques portables et faisant la valeur de référence** :

1. le **code des fonctions** (téléchargé **verbatim** du dépôt officiel, jamais réécrit) ;
2. la **génération d'inputs** (`input.py` SeBS → nos `inputs/*.json` + `seed-storage.py`) ;
3. le **wrapper de mesure** SeBS (begin/end, `results_time`, `is_cold`, bloc
   `measurement`) — reproduit fidèlement dans l'hôte `template/server.py`
   (d'après `benchmarks/wrappers/openwhisk/python/__main__.py`).

On emballe ça dans un **hôte Python + watchdog** (exactement comme `macro-befaas-iot`
le fait pour Node), et on **pilote avec NOTRE harnais** (wrk2, CPU 400 %, etc.).
La migration est **agnostique au langage** (le watchdog full-proxy relaie vers un
upstream HTTP local) → les fonctions Python tournent **sans modifier le watchdog**.

## Les 4 applications (faibles ressources, aspects variés)

| Fonction | SeBS | Aspect évalué | Stockage |
|---|---|---|---|
| `dynamic-html`  | 110.dynamic-html  | CPU / templating (web)        | aucun |
| `thumbnailer`   | 210.thumbnailer   | CPU image + I/O objet         | MinIO |
| `compression`   | 311.compression   | CPU compression + I/O objet   | MinIO |
| `graph-pagerank`| 501.graph-pagerank| CPU calcul pur (igraph)       | aucun |

Contrairement à BeFaaS, ces fonctions **n'appellent pas d'autres fonctions** :
1 seul hop client↔fonction → on **isole proprement** le coût du chemin de données
(migration vs proxy), sans confondre avec une chaîne applicative.

## Arborescence

```
sebs-bench/
  functions/<fn>/function/   # code SeBS VERBATIM (function.py [+ storage.py, templates/])
  functions/<fn>/requirements.txt
  template/server.py         # hôte Python + wrapper de mesure SeBS (gunicorn)
  template/requirements.txt  # flask + gunicorn
  inputs/<fn>.json           # l'event POSTé (dérivé des input.py SeBS)
  Dockerfile.vanilla / Dockerfile.fullproxy
  build-all.sh
  deploy/  stack-vanilla.yml  stack-proto.yml  minio-compose-snippet.yml
           seed-storage.py    deploy-all.sh
  eval/    client/post_json.lua  run_sebs_sweep.py  run_perfcost.py
```

---

## 0. Pré-requis (une fois)

- `docker buildx` configuré `linux/arm64` (machine de build = client).
- `wrk2` côté client ; SSH par clé vers le Pi (`romero@192.168.2.2`).
- Secrets `server-crt` / `server-key` présents sur le Pi (déjà le cas).
- **MinIO** déployé sur le Pi (pour thumbnailer + compression) :
  ```bash
  # sur le Pi
  sudo nano /var/lib/faasd/docker-compose.yaml      # coller deploy/minio-compose-snippet.yml dans services:
  sudo systemctl restart faasd
  # seeding des inputs (pip3 install --break-system-packages minio pillow) :
  python3 deploy/seed-storage.py                    # crée le bucket + dépose image & dataset
  ```

> `dynamic-html` et `graph-pagerank` n'ont besoin **ni de MinIO ni de seeding**.

---

## 1. Build + push des images (depuis la RACINE du dépôt)

```bash
cd <repo-root>
SB=benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench

# proto (les 4) — le 1er build compile wolfSSL (~13 min sous émulation arm64) :
./$SB/build-all.sh proto
# vanilla (les 4) :
./$SB/build-all.sh vanilla
# (une seule : ./$SB/build-all.sh proto thumbnailer)
```
Images : `romerosdd/sebs-{vanilla,fullproxy}-{dynamic-html,thumbnailer,compression,graph-pagerank}:latest`.

> **igraph (pagerank)** : `pip install igraph` tire normalement une wheel arm64
> (pas de compilation). Si le build échoue dessus, voir la note en bas.

---

## 2. Déployer (sur le Pi)

Pas de http/https au déploiement : **une image proto sert les deux** (le schéma se
choisit côté client à l'éval). Déploiement **séquentiel** (évite la race de
snapshots faasd).

```bash
cd .../sebs-bench/deploy
faas-cli list --gateway http://127.0.0.1:8080         # les 4 fonctions Ready

# test direct (doit donner du JSON wrapper : begin/end/results_time/is_cold) :
curl -s http://127.0.0.1:8080/function/dynamic-html \
  -H 'Content-Type: application/json' -d '{"username":"pi","random_len":5}' | head -c 300
```
> vanilla et proto ont les **mêmes noms** → on ne peut pas avoir les deux en même
> temps : déployer un KIND → mesurer → déployer l'autre → mesurer.

---

## 3. Éval A — rate-sweep (RPS, latences, CPU 400 %)

`run_sebs_sweep.py` poste l'event JSON à débit constant (wrk2) et relève RPS,
latences, débit, erreurs + **CPU global du Pi (échelle 400 %)**. Le CSV a les
**mêmes colonnes que `compare_two_csv_plots.py`** → nos plots se réutilisent.

```bash
cd .../sebs-bench/eval

# exemple : dynamic-html, proto puis vanilla, en HTTPS
python3 run_sebs_sweep.py --mode proto   --scheme https --host 192.168.2.2 \
  --function dynamic-html --input ../inputs/dynamic-html.json \
  --rates 5,10,20,40,60,80 --concurrency 16 --duration-s 20 --pause 5 \
  --pi-ssh romero@192.168.2.2 --out results/proto_https_dynamic-html.csv
python3 run_sebs_sweep.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function dynamic-html --input ../inputs/dynamic-html.json \
  --rates 5,10,20,40,60,80 --concurrency 16 --duration-s 20 --pause 5 \
  --pi-ssh romero@192.168.2.2 --out results/vanilla_https_dynamic-html.csv

# plots (réutilise l'outil de macro-befaas / app_eval) :
python3 ../../app_eval/compare_two_csv_plots.py \
  results/vanilla_https_dynamic-html.csv results/proto_https_dynamic-html.csv \
  --label-a Vanilla --label-b Prototype \
  --prefix dynamic-html_https --out-dir plots/dynamic-html_https
```
Répéter par fonction (`--function thumbnailer --input ../inputs/thumbnailer.json`, …)
et par schéma (`--scheme http`, port 8080).

---

## 4. Éval B — perf-cost / overlay du chemin de données (LA métrique clé)

`run_perfcost.py` envoie des invocations **séquentielles**, lit le JSON du wrapper
SeBS, et décompose chaque appel :

```
client_ms   = latence end-to-end (mesurée côté client)
server_ms   = results_time du wrapper (temps DANS la fonction)
overhead_ms = client_ms - server_ms = réseau + watchdog + proxy/migration  ◄── Vanilla vs Proto
compute_ms  = measurement.compute_time (métier : resize/zip/pagerank)
is_cold     = cold start (1er appel d'un worker)
```

```bash
python3 run_perfcost.py --mode proto   --scheme https --host 192.168.2.2 \
  --function thumbnailer --input ../inputs/thumbnailer.json \
  --requests 100 --warmup 5 --out results/perfcost_proto_https_thumbnailer.csv
python3 run_perfcost.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function thumbnailer --input ../inputs/thumbnailer.json \
  --requests 100 --warmup 5 --out results/perfcost_vanilla_https_thumbnailer.csv
```
Le résumé imprime moy/p50/p99 de `client/server/overhead`. Comparer
`overhead_ms` proto vs vanilla = le gain net du chemin de données.

> **Cold start** : `--warmup 0` juste après un redéploiement → la 1re ligne aura
> `is_cold=True` (surcoût de premier appel).

---

## 5. Les métriques (les nôtres + SeBS)

1. **Les nôtres** (inchangées) : RPS, latences p50–p99, **CPU Pi 400 %** (avg/max),
   débit, erreurs/timeouts — via `run_sebs_sweep.py` (+ pidstat par composant si on
   réutilise `app_eval/sweep_app_wrk2.py`).
2. **SeBS** : `results_time` (serveur), `measurement.compute_time` (métier),
   `is_cold` — via le wrapper, exploités par `run_perfcost.py`.
3. **Dérivée clé** : `overhead = client − serveur` → isole Vanilla vs Prototype.

---

## 6. Matrice d'éval

| KIND | schéma client | port |
|---|---|---|
| vanilla | http  | 8080 |
| vanilla | https | 8443 |
| proto   | http  | 8080 |
| proto   | https | 8443 |

× 4 fonctions. Séquence par fonction : déployer proto → sweep+perfcost → déployer
vanilla → sweep+perfcost → plots ; en http puis https.

---

## 7. Notes

- **Concurrence modérée** + faasd stable (mêmes pièges qu'`app_eval`).
- **`storage.py`** (MinIO) est instancié à l'**import** de la fonction →
  `MINIO_STORAGE_*` doivent être dans l'ENV du conteneur (c'est le cas dans les
  stacks ; surchargeables via `MINIO_IP=` dans `deploy-all.sh`).
- **gunicorn** lance `GUNICORN_WORKERS` (def 4) workers process → vrai
  parallélisme multi-cœur (le GIL ne bride pas pagerank/thumbnailer).
- **igraph arm64** : si la wheel n'existe pas pour ta version, ajouter au
  Dockerfile (avant `pip install`) `apt-get install -y libigraph-dev` ou épingler
  une version d'`igraph` fournissant une wheel `manylinux2014_aarch64`.
