# app_eval — Évaluation macro de l'app BeFaaS IoT (vanilla vs prototype)

Évaluation de la **vraie application BeFaaS IoT** sur faasd, vanilla vs prototype
(migration `sendfd`). La charge tape le point d'entrée `objectrecognition` (POST
d'une image), qui déclenche la chaîne complète à 3 hops :

```
client ─► objectrecognition ─► emergencydetection ─► setlightphasecalculation
                            └─► trafficstatistics
```
Chaque hop inter-fonction repasse par le gateway → **migré** en proto, **re-proxifié** en vanilla.

> **Mesure CPU/RPS identique à `sebs-bench/eval`** : le CPU et le réseau du Pi sont
> relevés via **`sar`** (paquet sysstat) en une seule passe pendant chaque palier,
> et les courbes sont tracées avec **les mêmes scripts** que sebs
> (`plot_rps_cpu.py`, `plot_sebs_compare.py`).

Ce dossier contient **3 sous-benchs** :

| Sous-bench | Mesure | Script |
|---|---|---|
| **app_eval** (ce dossier) | rate-sweep : RPS, débit, latences p50–p99, **CPU + réseau du Pi via `sar`** (0–100 %, eth0 rx+tx) | `run_app_sweep.py` |
| **base_latence/** | latence end-to-end d'**1** requête (1 connexion TCP+TLS neuve/requête) vs **taille d'image** | `base_latence/run_base_latence.py` |
| **rate_vs_size/** | **RPS max sans erreur** pour chaque **taille d'image** | `rate_vs_size/run_rate_vs_size.py` |

---

## 0. Pré-requis (à lire une fois)

- **wrk2** installé côté client (auto-détecté dans `~/wrk2/wrk`, ou `WRK2=/chemin/wrk`).
- **`sar` (paquet `sysstat`) installé SUR LE PI** : `sudo apt-get install -y sysstat`.
- **SSH par clé** vers le Pi (`romero@192.168.2.2`) : le sweep lance `sar` à distance.
- Les scripts se lancent **depuis le LAPTOP** (le client). wrk2 tape le Pi et le
  script `ssh` sur le Pi pour relever CPU+réseau. **Ne pas lancer sur le Pi**
  (sinon client et serveur se disputent le CPU).
- Le **mode voulu doit être déployé** sur le Pi (voir §1).
- Python (plots) : `pip3 install --break-system-packages matplotlib numpy pandas`.

### ⚠️ Pièges appris (importants)
1. **Déployer une fonction à la fois.** `deploy-all.sh` le fait déjà (boucle
   `--filter`). Le déploiement groupé déclenche une race faasd. Si une fonction
   manque : `faas-cli deploy -f <stack> --filter <fn>`.
2. **Concurrence modérée** (`--concurrency 16`, pas 100/200). La chaîne a une
   latence intrinsèque élevée (verrou Redis + `setTimeout` + 3 hops) ; à forte
   concurrence on noie l'entrée `objectrecognition` → bruit.
3. **faasd ne doit pas flapper** pendant une campagne (EULA CE re-vérifiée via
   Internet). Vérifier `systemctl is-active faasd` stable.
4. **vanilla et proto = mêmes noms de fonctions** → pas les deux en même temps.
   Séquence : déployer un mode → sweeper → déployer l'autre → sweeper.

---

## 1. Déployer le mode voulu (sur le Pi)

```bash
# sur le Pi
cd ~/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/macro-befaas-iot/deploy

REDIS_IP=10.62.0.1 ./deploy-all.sh proto   https    # proto,   HTTPS (port 8443)
REDIS_IP=10.62.0.1 ./deploy-all.sh vanilla https    # vanilla, HTTPS
# (idem avec `http` pour le port 8080)

# vérifier que les 4 fonctions sont Ready :
faas-cli list --gateway http://127.0.0.1:8080
# et que l'entrée répond 200 :
curl -s http://127.0.0.1:8080/function/objectrecognition \
  -F "image=@/tmp/red.png" -o /dev/null -w '%{http_code}\n'
```

---

## 2. Lancer le rate-sweep (depuis le LAPTOP) — mesure `sar`

`run_app_sweep.py` : pour chaque débit cible, relève RPS / latences / erreurs
(wrk2) **et** CPU + réseau du Pi via **un seul `sar -u -n DEV`** échantillonné en
parallèle (`pi_cpu_busy_{avg,max}_pct` sur 0–100 %, `net_kb_s_{avg,max}` = rx+tx
sur eth0). Le **schéma CSV est identique à sebs** → mêmes scripts de plot.

```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/app_eval

# ===== PROTO / HTTPS =====   (après ./deploy-all.sh proto https)
python3 run_app_sweep.py --mode proto --scheme https \
  --host 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/proto_https_objreco_16c.csv

# ===== VANILLA / HTTPS =====   (après ./deploy-all.sh vanilla https)
python3 run_app_sweep.py --mode vanilla --scheme https \
  --host 192.168.2.2 --pi-ssh romero@192.168.2.2 \
  --rates 2,4,6,8,10,12,16,20 --concurrency 16 \
  --image images/image-ambulance.jpg --duration-s 20 --timeout-s 30 --pause 5 \
  --out results/vanilla_https_objreco_16c.csv

# (HTTP : remplacer --scheme https par --scheme http, port 8080)
```

Raccourci équivalent :
```bash
./run_eval.sh proto   https
./run_eval.sh vanilla https
```

Sortie : `results/<mode>_<scheme>_objreco_16c.csv` (1 ligne par débit). Colonnes :
`rate, rps, transfer_kb_s, net_kb_s_avg/max, pi_cpu_busy_avg/max_pct,
lat_avg/p50/p99_ms, client_ms_*, total_requests, socket_*_errors, …`.

> **Note perf-cost** : les colonnes `server_ms_*` / `overhead_ms_*` sont à **0**
> (BeFaaS ne renvoie pas de temps serveur, contrairement au wrapper SeBS). Seule
> la latence *client* (`client_ms_*`, mesurée par wrk2) est renseignée.

Paramètres ajustables : `--rates`, `--concurrency`, `--threads`, `--duration-s`,
`--timeout-s`, `--pause`, `--image` (voir §4), `--function`.

---

## 3. Plots vanilla vs proto (comme sebs-bench/eval)

Mêmes scripts que sebs, en mode **fichiers explicites** (`--vanilla` / `--proto`).

**Charge + CPU moyen (double axe, CPU gradué 0–100 %)** — génère **DEUX** figures :
```bash
python3 plot_rps_cpu.py --scheme https \
  --vanilla results/vanilla_https_objreco_16c.csv \
  --proto   results/proto_https_objreco_16c.csv
# -> results/rps_cpu_https/<dossier>_https_rps_cpu.png   (RPS atteint  + CPU)
# -> results/rps_cpu_https/<dossier>_https_lat_cpu.png   (latence moy. + CPU)
```
> Axe gauche : rouge = Vanilla, vert = Prototype. Axe droit : CPU du Pi
> (orange = Vanilla, bleu = Prototype, tireté), **toujours gradué jusqu'à 100 %**.
>
> **`--cpu-stat avg|med|q3`** (défaut `avg`) : choisit la statistique CPU tracée à
> droite — moyenne (`pi_cpu_busy_avg_pct`), médiane (`pi_cpu_busy_med_pct`) ou 3ᵉ
> quartile (`pi_cpu_busy_q3_pct`) — les **trois** étant collectées par le sweep. En
> `med`/`q3`, les fichiers sont suffixés `_med`/`_q3` (`…_rps_cpu_q3.png`, …) et ne
> remplacent donc pas la version `avg`.

**Comparaison multi-métriques** (RPS, CPU avg/max, réseau %, latences, requêtes…) :
```bash
python3 plot_sebs_compare.py --scheme https \
  --vanilla results/vanilla_https_objreco_16c.csv \
  --proto   results/proto_https_objreco_16c.csv \
  --label-a Vanilla --label-b Prototype
```
> 1er fichier = Vanilla, 2e = Prototype. `boxplot_sebs_compare.py` et
> `sum_sebs_compare.py` sont aussi présents (mêmes que sebs) ; ils utilisent la
> découverte par dossier `results/<app>/` (`--results-dir … --app …`).

---

## 4. Choix de l'image (impacte la latence)
- `images/image-ambulance.jpg` (**défaut**) : pixel rouge → emergency →
  `setlightphasecalculation` répond **vite**. Pour comparer le **coût réseau**
  (migration vs proxy) sans bruit applicatif.
- `images/image-noambulance.jpg` : non-emergency → chemin `waitAppropriately`
  (≥ 2 s) + verrou Redis (comportement BeFaaS réel). `--image images/image-noambulance.jpg`.

---

## 5. Les deux autres sous-benchs (inchangés)

### base_latence/ — latence end-to-end vs taille d'image (1 connexion/requête)
```bash
cd base_latence
python3 run_base_latence.py --mode proto --scheme https --host 192.168.2.2 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 --requests 50 --rate 2 \
  --output results/proto_https.csv
# vanilla idem -> results/vanilla_https.csv, puis :
python3 plot_eval.py results/proto_https.csv results/vanilla_https.csv \
  --output plots/latency_vs_payload.png
```

### rate_vs_size/ — RPS max sans erreur par taille d'image (+ CPU + réseau)
```bash
cd rate_vs_size
python3 run_rate_vs_size.py --mode proto --scheme http --host 192.168.2.2 \
  --pi-ssh romero@192.168.2.2 --nic-max-mbit 940 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 \
  --rates 2,4,6,8,10,12,16,20,30,40 --concurrency 16 \
  --duration-s 20 --timeout-s 70 --pause 5 --out results/proto_http.csv
# vanilla idem, puis :
python3 plot_rate_vs_size.py results/vanilla_http.csv results/proto_http.csv \
  --output plots/max_rps_vs_size_http.png
```

---

## 6. Notes méthodo
- wrk2 = charge à **débit constant** (open-loop) → latences corrigées de la
  coordinated-omission.
- **`concurrency ≥ RPS_visé × latence`** sinon on mesure la limite du *client*.
  Mais rester modéré (cf. piège 2).
- Le sweep re-lance automatiquement un palier si wrk2 renvoie une latence `-nan`
  (bug de durée coordinated-omission) en décalant la durée de +1 s.
- Le mode + le schéma doivent correspondre au **déploiement courant** sur le Pi.

> **Historique** : l'ancien sweep `sweep_app_wrk2.py` (CPU par composant via
> `pidstat`) et son plot `plot_pidstat_cpu.py` restent présents pour un
> découpage CPU *par processus* (gateway / faasd / fwatchdog / worker), mais la
> mesure CPU/RPS de référence est désormais `run_app_sweep.py` (`sar`, comme sebs).
