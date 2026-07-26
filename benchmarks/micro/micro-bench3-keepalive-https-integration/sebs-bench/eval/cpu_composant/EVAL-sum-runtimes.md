# CPU par composant — fonctions `sum` Python & JS (vanilla vs proto)

Même bench que ce dossier (`run_sweep_cpu.py` + `plot_cpu_pies.py`), appliqué aux
4 nouvelles fonctions. Résultats **classés par runtime**, avec les deux modes
côte à côte pour que le camembert compare **vanilla vs proto** directement :

```
results/
  sum-python/
    vanilla.csv                      # CSV principal (RPS + CPU par composant)
    proto.csv
    pidstat/vanilla/cpu/*.csv        # 1 fichier par composant, % du Pi
    pidstat/prototype/cpu/*.csv
    cpu_pies.png                     # camembert vanilla | proto
  sum-js/
    vanilla.csv  proto.csv
    pidstat/{vanilla,prototype}/cpu/*.csv
    cpu_pies.png
```

| runtime | route vanilla | route proto | process métier (worker) |
|---|---|---|---|
| Python | `sum-python-vanilla` | `sum-python-proto` | gunicorn (maître + 4 workers) |
| JS | `sum-js-vanilla` | `sum-js-proto` | node |

> Le résolveur pidstat capture désormais **tout le sous-arbre** du fwatchdog →
> pour Python, les 4 workers gunicorn sont bien comptés (vérifié : 5 PID worker).

Pré-requis : images buildées + poussées (`proto_function/build-all.sh all`),
secrets TLS `server-crt`/`server-key`, `sar`+`sudo` NOPASSWD sur le Pi, `wrk2` client.

---

## Phase 1 — VANILLA (gateway `HTTPMIGRATE_ENABLE=0`)

### 1a. Sur le Pi
```bash
sudo sed -i 's/HTTPMIGRATE_ENABLE=1/HTTPMIGRATE_ENABLE=0/' /var/lib/faasd/docker-compose.yaml
sudo systemctl restart faasd
cd ~/…/deploy
faas-cli deploy -f sum-python-vanilla.yml ; sleep 3
faas-cli deploy -f sum-js-vanilla.yml
# smoke test
curl -sk -X POST https://192.168.2.2:8443/function/sum-python-vanilla -d '3 4'   # -> result 7
curl -sk -X POST https://192.168.2.2:8443/function/sum-js-vanilla      -d '3 4'   # -> result 7
```

### 1b. Sur le client
```bash
cd ~/…/sebs-bench/eval/cpu_composant

python3 run_sweep_cpu.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function sum-python-vanilla --input ../../inputs/sumprod.txt \
  --rates 64,128,192,256,384,512,768 --conn-mode keepalive \
  --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/sum-python/vanilla.csv --pidstat-dir results/sum-python/pidstat

python3 run_sweep_cpu.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function sum-js-vanilla --input ../../inputs/sumprod.txt \
  --rates 64,128,192,256,384,512,768 --conn-mode keepalive \
  --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/sum-js/vanilla.csv --pidstat-dir results/sum-js/pidstat
```

---

## Phase 2 — PROTO (gateway `HTTPMIGRATE_ENABLE=1`)

### 2a. Sur le Pi
```bash
sudo sed -i 's/HTTPMIGRATE_ENABLE=0/HTTPMIGRATE_ENABLE=1/' /var/lib/faasd/docker-compose.yaml
sudo systemctl restart faasd
cd ~/…/deploy
faas-cli deploy -f sum-python-proto.yml ; sleep 3
faas-cli deploy -f sum-js-proto.yml
curl -sk -X POST https://192.168.2.2:8443/function/sum-python-proto -d '3 4'   # -> result 7
curl -sk -X POST https://192.168.2.2:8443/function/sum-js-proto      -d '3 4'   # -> result 7
```

### 2b. Sur le client — MÊME `--pidstat-dir` que la phase vanilla (→ 2 modes ensemble)
```bash
python3 run_sweep_cpu.py --mode proto --scheme https --host 192.168.2.2 \
  --function sum-python-proto --input ../../inputs/sumprod.txt \
  --rates 64,128,192,256,384,512,768 --conn-mode keepalive \
  --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/sum-python/proto.csv --pidstat-dir results/sum-python/pidstat

python3 run_sweep_cpu.py --mode proto --scheme https --host 192.168.2.2 \
  --function sum-js-proto --input ../../inputs/sumprod.txt \
  --rates 64,128,192,256,384,512,768 --conn-mode keepalive \
  --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/sum-js/proto.csv --pidstat-dir results/sum-js/pidstat
```

---

## Phase 3 — Camemberts (vanilla vs proto par runtime)

```bash
python3 plot_cpu_pies.py --pidstat-dir results/sum-python/pidstat \
  --out results/sum-python/cpu_pies.png
python3 plot_cpu_pies.py --pidstat-dir results/sum-js/pidstat \
  --out results/sum-js/cpu_pies.png

# (option) ne moyenner que la zone de saturation (~95% CPU), ex. débits >= 384 :
python3 plot_cpu_pies.py --pidstat-dir results/sum-python/pidstat --min-rate 384 \
  --out results/sum-python/cpu_pies_sat.png
```

---

## Notes importantes
- **Un seul mode gateway à la fois** : on ne mélange pas vanilla et proto (le
  gateway est soit en migration soit en relais). D'où les 2 phases distinctes.
- **Même `--pidstat-dir` par runtime** pour les 2 modes → le camembert affiche
  `vanilla | prototype` côte à côte. Le total de chaque camembert = le CPU sar
  du palier (pidstat ÷ ncores + reste système).
- **500 Ko** : refaire avec `--input ../../inputs/sumprod_500kb.txt` et un
  `--pidstat-dir` distinct (ex. `results/sum-python-500kb/pidstat`) pour voir le
  glissement de coût (copie/TLS) vers la gateway.
- Le CPU worker Python (gunicorn ×N) sera plus élevé que node/C : c'est
  l'intérêt de la comparaison inter-runtime.
