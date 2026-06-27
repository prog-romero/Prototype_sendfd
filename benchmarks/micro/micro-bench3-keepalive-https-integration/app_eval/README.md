# app_eval — Évaluation macro de l'app BeFaaS IoT (vanilla vs prototype)

Évaluation de la **vraie application BeFaaS IoT** sur faasd, vanilla vs prototype
(migration `sendfd`). La charge tape le point d'entrée `objectrecognition`, qui
déclenche la chaîne complète à 3 hops :

```
client ─► objectrecognition ─► emergencydetection ─► setlightphasecalculation
                            └─► trafficstatistics
```
Chaque hop inter-fonction repasse par le gateway → **migré** en proto, **re-proxifié** en vanilla.

Ce dossier contient **3 sous-benchs** + leurs scripts de plot :

| Sous-bench | Mesure | Script |
|---|---|---|
| **app_eval** (ce dossier) | rate-sweep : RPS, débit, latences p50–p99, **CPU avg/max du Pi** + **pidstat par composant** | `sweep_app_wrk2.py` |
| **base_latence/** | latence end-to-end d'**1** requête (1 connexion TCP+TLS neuve/requête) vs **taille d'image** | `base_latence/run_base_latence.py` |
| **rate_vs_size/** | **RPS max sans erreur** pour chaque **taille d'image** | `rate_vs_size/run_rate_vs_size.py` |

---

## 0. Pré-requis (à lire une fois)

- **wrk2** installé côté client (auto-détecté dans `~/wrk2/wrk`, ou `WRK2=/chemin/wrk`).
- **SSH par clé** vers le Pi (`romero@192.168.2.2`) — nécessaire pour le monitoring CPU/pidstat.
- Les scripts se lancent **depuis le LAPTOP** (le client). wrk2 tape le Pi et le
  script ssh sur le Pi pour relever le CPU. **Ne pas lancer sur le Pi** (sinon le
  client et le serveur se disputent le CPU).
- Le **mode voulu doit être déployé** sur le Pi (voir §1).

### ⚠️ Pièges appris (importants)
1. **Déployer une fonction à la fois.** `deploy-all.sh` le fait déjà (boucle
   `--filter`). Le déploiement groupé déclenche une race faasd (collision de
   snapshots → une fonction ne se crée pas). Si une fonction manque malgré tout :
   `faas-cli deploy -f <stack> --filter <fn>`.
2. **Concurrence modérée** (`--concurrency 16`, pas 100/200). La chaîne a une
   latence intrinsèque élevée (verrou Redis + `setTimeout` de
   setlightphasecalculation + 3 hops migrés) ; à forte concurrence on noie
   l'entrée `objectrecognition` (jimp) → elle tombe (`code=000`) et les chiffres
   deviennent du bruit.
3. **faasd ne doit pas flapper** pendant une campagne : faasd CE re-vérifie l'EULA
   via Internet ; si l'Internet du Pi blippe, faasd redémarre → le gateway tombe
   → `socket_connect_errors`. Vérifier `systemctl is-active faasd` stable.
4. **vanilla et proto = mêmes noms de fonctions** → on ne peut pas avoir les deux
   déployés en même temps. Séquence : déployer un mode → sweeper → déployer
   l'autre → sweeper. Pour vanilla il faut avoir buildé+déployé les images
   `iot-vanilla-*` (sinon on mesure du proto étiqueté « vanilla »).

---

## 1. Déployer le mode voulu (sur le Pi)

```bash
# sur le Pi
cd ~/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/macro-befaas-iot/deploy

REDIS_IP=10.62.0.1 ./deploy-all.sh proto   http     # proto,   HTTP  (port 8080)
REDIS_IP=10.62.0.1 ./deploy-all.sh proto   https    # proto,   HTTPS (port 8443)
REDIS_IP=10.62.0.1 ./deploy-all.sh vanilla http     # vanilla, HTTP
REDIS_IP=10.62.0.1 ./deploy-all.sh vanilla https    # vanilla, HTTPS

# vérifier que les 4 fonctions sont Ready :
faas-cli list --gateway http://127.0.0.1:8080
# et que l'entrée répond (doit donner 200) :
curl -s http://127.0.0.1:8080/function/objectrecognition \
  -F "image=@/tmp/red.png" -o /dev/null -w '%{http_code}\n'
```

---

## 2. Lancer le rate-sweep (depuis le LAPTOP)

`sweep_app_wrk2.py` : pour chaque débit, RPS/latences/erreurs + CPU Pi global +
pidstat par composant (gateway, faasd, fwatchdog-<fn>, worker-<fn>).

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/app_eval

# ===== PROTO / HTTP =====   (après ./deploy-all.sh proto http sur le Pi)
python3 sweep_app_wrk2.py --mode proto --scheme http \
  --gateway-ip 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/proto_http_objreco_16c.csv

# ===== VANILLA / HTTP =====   (après ./deploy-all.sh vanilla http)
python3 sweep_app_wrk2.py --mode vanilla --scheme http \
  --gateway-ip 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/vanilla_http_objreco_16c.csv

# ===== PROTO / HTTPS =====   (après ./deploy-all.sh proto https)
python3 sweep_app_wrk2.py --mode proto --scheme https \
  --gateway-ip 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/proto_https_objreco_16c.csv

# ===== VANILLA / HTTPS =====   (après ./deploy-all.sh vanilla https)
python3 sweep_app_wrk2.py --mode vanilla --scheme https \
  --gateway-ip 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/vanilla_https_objreco_16c.csv
```

Sorties :
- CSV résumé : `results/<mode>_<scheme>_objreco_16c.csv` (1 ligne par débit).
- pidstat par composant : `pidstat/<mode>/{cpu,ram}/<composant>.csv`
  (`<mode>` = `vanilla` ou **`prototype`**).

Paramètres ajustables : `--rates`, `--concurrency`, `--duration-s`, `--timeout-s`,
`--threads`, `--pause`, `--image` (voir §5), `--function`.

---

## 3. Plots de comparaison vanilla vs proto (6 métriques)

`compare_two_csv_plots.py` — 6 figures : **rps**, **débit**, **cpu avg**,
**cpu max**, **latence avg**, **total requests**. Le merge est en **OUTER join** :
si un mode a moins de paliers (p. ex. lignes en erreur supprimées à la main), les
paliers de l'autre mode s'affichent quand même (barre absente là où il manque).

```bash
# --- HTTPS ---
python3 compare_two_csv_plots.py \
  results/vanilla_https_objreco_16c.csv results/proto_https_objreco_16c.csv \
  --label-a Vanilla --label-b Prototype \
  --prefix https_objreco_16c --out-dir plots/https_objreco_16c

# --- HTTP ---
python3 compare_two_csv_plots.py \
  results/vanilla_http_objreco_16c.csv results/proto_http_objreco_16c.csv \
  --label-a Vanilla --label-b Prototype \
  --prefix http_objreco_16c --out-dir plots/http_objreco_16c
```
> 1er fichier = `--label-a` (Vanilla), 2e = `--label-b` (Prototype) — garde cet ordre.

---

## 4. Plots CPU par composant (courbes + camemberts)

`plot_pidstat_cpu.py` — lit `pidstat/<mode>/cpu/*.csv` (colonne `cpu_pct` = CPU
total du composant) et produit **3 images** :
- `cpu_lines_vanilla.png` et `cpu_lines_prototype.png` : **une image par mode**
  (grandes, plus lisibles), avec la **MÊME échelle Y** → comparables côte à côte.
  Même couleur = même composant dans les deux.
- `cpu_pies.png` : 2 camemberts rapprochés (vanilla / prototype) — part **moyenne**
  (sur tous les débits) de CPU de chaque composant. Pas de % à l'intérieur ;
  grande légende couleurs+noms en dessous.

```bash
python3 plot_pidstat_cpu.py --pidstat-dir pidstat --out-dir plots/pidstat_cpu
```
> Suppose que `pidstat/vanilla/cpu/` ET `pidstat/prototype/cpu/` existent (donc
> avoir sweepé les deux modes). Sous-dossiers personnalisables : `--vanilla-name`,
> `--proto-name`.

---

## 5. Choix de l'image (impacte la latence)
- `images/image-ambulance.jpg` (**défaut**) : pixel rouge → emergency →
  `setlightphasecalculation` répond **vite** (pas de `setTimeout`). Pour comparer
  le **coût réseau** (migration vs proxy) sans bruit applicatif.
- `images/image-noambulance.jpg` : non-emergency → peut emprunter le chemin
  `waitAppropriately` (≥ 2 s) + verrou Redis (comportement BeFaaS réel). Pour
  inclure ce coût applicatif. `--image images/image-noambulance.jpg`.

---

## 6. Les deux autres sous-benchs

### base_latence/ — latence end-to-end vs taille d'image (1 connexion/requête)
```bash
cd base_latence
# proto HTTPS, toutes les tailles (images générées 2..1024 KB) :
python3 run_base_latence.py --mode proto --scheme https --host 192.168.2.2 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 --requests 50 --rate 2 \
  --output results/proto_https.csv
# vanilla idem -> results/vanilla_https.csv, puis :
python3 plot_eval.py results/proto_https.csv results/vanilla_https.csv \
  --output plots/latency_vs_payload.png
```

### rate_vs_size/ — RPS max sans erreur par taille d'image (+ CPU + réseau)

Pour CHAQUE taille d'image, balaie les débits, retient le **RPS max sans erreur**,
et **au palier de ce max** relève AUSSI :
- le **CPU total du Pi** (échantillonné via `--pi-ssh`, **échelle 400 %** = 4 cœurs),
  avg ET max ;
- le **débit réseau réel** = **upload de l'image** (`rps × taille`, que wrk2 ne
  compte pas) **+ download** (Transfer/sec de wrk2), puis le **% du débit max de
  la carte réseau** (`--nic-max-mbit`, def **940 Mbit/s** mesuré via iperf3).

But : voir si on est limité par **le CPU** ou **la carte réseau** du Pi.

```bash
cd rate_vs_size
# un run = toutes les tailles d'un coup (mêmes --rates, --concurrency pour comparer) :
python3 run_rate_vs_size.py --mode proto --scheme http --host 192.168.2.2 \
  --pi-ssh romero@192.168.2.2 --nic-max-mbit 940 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 \
  --rates 2,4,6,8,10,12,16,20,30,40 --concurrency 16 \
  --duration-s 20 --timeout-s 70 --pause 5 --out results/proto_http.csv
# (workflow taille-par-taille : ajouter --append et changer --sizes à chaque run)
# vanilla idem -> results/vanilla_http.csv, puis histogramme + courbe :
python3 plot_rate_vs_size.py results/vanilla_http.csv results/proto_http.csv \
  --output plots/max_rps_vs_size_http.png
```

> Sur le plot, au-dessus de chaque barre : `RPS` puis `C avg/max%` (CPU Pi sur
> 400 %) puis `N %` (débit carte réseau atteint). Colonnes ajoutées au CSV résumé :
> `cpu_avg_pct`, `cpu_max_pct`, `upload_mbit_s`, `download_mbit_s`, `net_mbit_s`,
> `net_pct`, `nic_max_mbit` (les anciens CSV sans ces colonnes restent traçables :
> les annotations CPU/NET valent alors 0).

---

## 7. Notes méthodo
- wrk2 = charge à **débit constant** (open-loop) → latences corrigées de la
  coordinated-omission.
- **`concurrency ≥ RPS_visé × latence`** sinon on mesure la limite du *client*
  (nb de connexions / latence), pas du serveur. Mais rester modéré (cf. piège 2).
- Démarrer modeste, monter les débits jusqu'à voir la saturation (erreurs/timeouts).
- Le mode + le schéma doivent correspondre au **déploiement courant** sur le Pi.
