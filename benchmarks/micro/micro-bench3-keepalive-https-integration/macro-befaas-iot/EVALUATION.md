# Évaluation macro BeFaaS-IoT (vanilla vs prototype, tout HTTPS)

Application « smart-traffic » à 4 fonctions Node.js + Redis. Une requête client
déclenche une chaîne d'appels inter-fonctions (voir `workflow.puml`) :

```
client → objectrecognition ──call──> trafficstatistics ──> Redis
                            └─call──> emergencydetection ──call──> setlightphasecalculation ──> Redis
```

**Communication inter-fonctions = HTTPS dans les DEUX modes** (port 8443) :
- **vanilla** : le gateway termine TLS puis relaie (gateway→provider→watchdog).
- **proto**   : le gateway migre la connexion via sendfd (le container répond direct).

> ⚠️ Chaque `ctx.call()` de BeFaaS (node-fetch) est **non-keepalive** : une
> connexion TCP neuve par appel. Une requête client = **3 handshakes TLS**
> inter-fonctions (OR→TS, OR→ED, ED→SL). C'est ce coût que le proto allège.

Registry par défaut : `romerosdd`. IP Pi : `192.168.2.2`. SSH Pi : `romero@192.168.2.2`.

---

## 1. Build des images (machine dev, depuis la RACINE du dépôt)

```bash
cd <repo-root>
./benchmarks/micro/micro-bench3-keepalive-https-integration/macro-befaas-iot/build-all.sh all
#   -> romerosdd/iot-vanilla-<fn>:latest  et  romerosdd/iot-fullproxy-<fn>:latest
#   (fn = objectrecognition, emergencydetection, trafficstatistics, setlightphasecalculation)
```
Options : `REGISTRY=…`, `PLATFORM=linux/arm64`, `PUSH=0` (--load au lieu de --push).

---

## 2. Redis (une fois, sur le Pi)

Publie Redis sur le bridge faasd `10.62.0.1:6379` (IP stable, joignable par les
fonctions). Si Redis est déjà dans le `docker-compose.yaml` de faasd, corrige juste
la ligne `ports:` ; sinon ajoute le bloc de `deploy/redis-compose-snippet.yml` :

```bash
sudo sed -i 's#- "6379:6379"#- "10.62.0.1:6379:6379"#' /var/lib/faasd/docker-compose.yaml
sudo systemctl restart faasd
# vérif
redis-cli -h 10.62.0.1 ping     # -> PONG
```

---

## 3. Choisir le mode du gateway (tout HTTPS)

Le gateway est piloté par 2 env dans `/var/lib/faasd/docker-compose.yaml` :
`HTTPS_ENABLE=1` (toujours, pour le 8443) et `HTTPMIGRATE_ENABLE` (0=vanilla, 1=proto).

```bash
# VANILLA (HTTPS terminé + relayé)
sudo sed -i 's/HTTPMIGRATE_ENABLE=1/HTTPMIGRATE_ENABLE=0/' /var/lib/faasd/docker-compose.yaml
# … s'assurer que HTTPS_ENABLE=1 est présent sur le service gateway …
sudo systemctl restart faasd

# PROTO (migration sendfd) — quand tu passeras au proto
sudo sed -i 's/HTTPMIGRATE_ENABLE=0/HTTPMIGRATE_ENABLE=1/' /var/lib/faasd/docker-compose.yaml
sudo systemctl restart faasd
```

---

## 4. Déployer les 4 fonctions (sur le Pi) — endpoint HTTPS

`deploy-all.sh <vanilla|proto> <http|https>` réécrit `OPENFAAS_ENDPOINT` et
`REDIS_ENDPOINT`, puis déploie **une fonction à la fois** (évite la race de
snapshots containerd). On utilise **`https`** pour l'inter-fonctions.

```bash
cd ~/…/macro-befaas-iot/deploy

# force le re-pull des images (faasd cache :latest)
for fn in objectrecognition emergencydetection trafficstatistics setlightphasecalculation; do
  sudo ctr -n openfaas-fn image rm docker.io/romerosdd/iot-vanilla-$fn:latest 2>/dev/null || true
  sudo ctr -n openfaas-fn image rm docker.io/romerosdd/iot-fullproxy-$fn:latest 2>/dev/null || true
done

./deploy-all.sh vanilla https        # (ou: proto https)
faas-cli list                         # les 4 fonctions doivent apparaître
```

**Smoke test** (l'entrée = objectrecognition, POST d'une image) :
```bash
curl -sk -X POST https://192.168.2.2:8443/function/objectrecognition \
  -F image=@../app_eval/images/image-ambulance.jpg
# -> JSON (objects…). Vérifie Redis :
redis-cli -h 10.62.0.1 keys 'trafficstatistics-*' | head
redis-cli -h 10.62.0.1 get lightcalculation:lights
```

---

## 5. Lancer le sweep de charge (depuis le CLIENT)

`run_app_sweep.py` envoie l'image en open-loop (wrk2) et mesure RPS + CPU/réseau
du Pi (`sar`). Le CSV est au même format que `run_sebs_sweep.py` → plots réutilisables.

```bash
cd ~/…/app_eval
pip3 install --break-system-packages matplotlib numpy pandas 2>/dev/null || true

# VANILLA (HTTPS)
python3 run_app_sweep.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function objectrecognition --image images/image-ambulance.jpg \
  --rates 5,10,15,20,25,30,40,50 --concurrency 8 --threads 8 --duration-s 20 \
  --pi-ssh romero@192.168.2.2 --out results/vanilla_https_objreco.csv

# PROTO (HTTPS) — après avoir rebasculé le gateway (§3) + redéployé en proto (§4)
python3 run_app_sweep.py --mode proto --scheme https --host 192.168.2.2 \
  --function objectrecognition --image images/image-ambulance.jpg \
  --rates 5,10,15,20,25,30,40,50 --concurrency 8 --threads 8 --duration-s 20 \
  --pi-ssh romero@192.168.2.2 --out results/proto_https_objreco.csv
```

> On ne mélange pas les modes : une passe vanilla (gateway `HTTPMIGRATE_ENABLE=0`
> + fonctions `iot-vanilla-*`), puis une passe proto (`=1` + `iot-fullproxy-*`).

---

## 6. Tracer / comparer

```bash
# RPS + CPU vs débit, vanilla vs proto sur le même graphe
python3 compare_two_csv_plots.py \
  results/vanilla_https_objreco.csv results/proto_https_objreco.csv \
  --labels vanilla proto --out plots/objreco_vanilla_vs_proto.png
```

### (option) CPU PAR COMPOSANT
Pour la répartition gateway/faasd-provider/containerd/… sur BeFaaS, utilise
`sweep_app_wrk2.py` (même sweep + `pidstat` par composant, résout les PID des 4
fonctions IoT). Voir aussi `sebs-bench/eval/cpu_composant/` pour le camembert
(pense à normaliser en % du Pi ÷ ncores si tu réutilises ce plot).

---

## Récap matrice

| Mode | Gateway env | Images | deploy | Client |
|---|---|---|---|---|
| vanilla | `HTTPMIGRATE_ENABLE=0` `HTTPS_ENABLE=1` | `iot-vanilla-*` | `deploy-all.sh vanilla https` | `--mode vanilla --scheme https` |
| proto   | `HTTPMIGRATE_ENABLE=1` `HTTPS_ENABLE=1` | `iot-fullproxy-*` | `deploy-all.sh proto https` | `--mode proto --scheme https` |

## Pré-requis
- `sar` (`sysstat`) sur le Pi, SSH sans mot de passe (sudo NOPASSWD pour le CPU par composant).
- `wrk2` sur le client (`~/wrk2/wrk`).
- Certificat gateway auto-signé → les fonctions ont `NODE_TLS_REJECT_UNAUTHORIZED=0` (déjà dans les stacks).
