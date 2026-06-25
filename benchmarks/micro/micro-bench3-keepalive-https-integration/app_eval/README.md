# app_eval — Évaluation macro de l'app BeFaaS IoT (vanilla vs prototype)

Même méthodologie et **mêmes métriques** que `../evaluation_throughput` (rate-sweep
wrk2, latences p50–p99, RPS, débit, CPU Pi global + pidstat par composant), mais
appliquée à la **vraie application BeFaaS IoT** : la charge tape le point d'entrée
`objectrecognition`, qui déclenche la chaîne complète à 3 hops :

```
objectrecognition ─► emergencydetection ─► setlightphasecalculation
                  └─► trafficstatistics
```

Chaque hop inter-fonction repasse par le gateway → **migré** en mode prototype,
**re-proxifié** en vanilla.

## Différences avec evaluation_throughput
- Cible = `objectrecognition` (point d'entrée unique), pas l'alternance fn-a/fn-b.
- Requête = **POST multipart d'une image** (`client/post_image.lua`) — c'est ce
  qu'attend `objectrecognition` (`upload.single('image')` + `jimp`).
- `--scheme http|https` (port 8080 / 8443).
- vanilla et proto déploient le **même nom** de fonction → l'URL est identique ;
  `--mode` ne sert qu'au nom de fichier de sortie et au pidstat.

## Pré-requis
- wrk2 installé (auto-détecté dans `~/wrk2/wrk`, ou via `WRK2=/chemin/wrk`).
- Accès SSH au Pi (clé) pour le monitoring CPU/pidstat (`PI_SSH=romero@192.168.2.2`).
- Les fonctions du mode voulu sont **déployées** sur le Pi (voir
  `../macro-befaas-iot/README.md`).

## Lancer une évaluation

`run_eval.sh <vanilla|proto> <http|https> [rates] [concurrency]` :

```bash
cd app_eval

# ── Évaluation HTTPS ──
# 1) déployer vanilla https (côté Pi), puis :
./run_eval.sh vanilla https
# 2) déployer proto https (côté Pi), puis :
./run_eval.sh proto   https

# ── Évaluation HTTP ──
./run_eval.sh vanilla http
./run_eval.sh proto   http
```

Sorties CSV → `results/<mode>_<scheme>_objreco_100c.csv` (colonnes identiques à
evaluation_throughput). pidstat par composant → `pidstat/<mode>/{cpu,ram}/*.csv`.

> Important : vanilla et proto utilisent les mêmes noms de fonctions, donc on ne
> peut pas avoir les deux déployés en même temps. Séquence : **déployer un mode →
> sweeper → déployer l'autre → sweeper**.

### Appel direct du script (plus d'options)
```bash
python3 sweep_app_wrk2.py --mode proto --scheme https \
  --rates 25,50,75,100,150,200 --concurrency 100 \
  --image images/image-ambulance.jpg --duration-s 20 \
  --out results/proto_https_objreco_100c.csv
```

## Générer les plots de comparaison

`compare_two_csv_plots.py` (identique à evaluation_throughput) — 6 figures
(rps, débit, cpu avg/max, latence avg, total requests) :

```bash
# HTTPS
python3 compare_two_csv_plots.py \
  results/vanilla_https_objreco_100c.csv \
  results/proto_https_objreco_100c.csv \
  --label-a Vanilla --label-b Prototype \
  --prefix https_objreco_100c --out-dir plots/https_objreco_100c

# HTTP
python3 compare_two_csv_plots.py \
  results/vanilla_http_objreco_100c.csv \
  results/proto_http_objreco_100c.csv \
  --label-a Vanilla --label-b Prototype \
  --prefix http_objreco_100c --out-dir plots/http_objreco_100c
```

## Choix de l'image (important pour la latence)
- `images/image-ambulance.jpg` (**défaut**) : pixel rouge → emergency →
  `setlightphasecalculation` répond **vite** (pas de `setTimeout`). À utiliser
  pour comparer le **coût réseau** (migration vs proxy) sans bruit applicatif.
- `images/image-noambulance.jpg` : non-emergency → peut emprunter le chemin
  `waitAppropriately` (≥ 2 s) + verrou Redis de `setlightphasecalculation`
  (comportement BeFaaS réel). À utiliser si vous voulez inclure ce coût applicatif.
  `IMAGE=images/image-noambulance.jpg ./run_eval.sh proto https`

## Notes méthodo
- wrk2 = charge à **débit constant** (open-loop) → latences corrigées de la
  coordinated-omission, comme pour les micro-benchs.
- Démarrer par des débits modestes (l'app fait du `jimp` + 3 hops TLS par requête
  sur un Pi) ; augmenter jusqu'à voir la saturation (erreurs/timeouts).
- Le mode (vanilla/proto) et le schéma (http/https) doivent correspondre au
  déploiement courant sur le Pi.
