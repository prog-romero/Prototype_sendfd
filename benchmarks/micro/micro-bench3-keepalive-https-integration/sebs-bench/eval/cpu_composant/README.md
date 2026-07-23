# cpu_composant — CPU par composant (gateway / faasd récupérables via sendfd)

But : montrer **combien de CPU la gateway et le provider faasd consomment** dans le
chemin **vanilla** (ils traitent/proxy CHAQUE requête). C'est exactement le CPU que
**sendfd court-circuite** : une fois la connexion migrée, gateway et provider sortent
du chemin par-requête → ce CPU est rendu au container pour servir plus de requêtes.

Ce dossier ne modifie pas `run_sebs_sweep.py`. Il contient :
- **`run_sweep_cpu.py`** — copie de `run_sebs_sweep.py` + collecte du **CPU par composant**
  via `pidstat`, en parallèle de chaque palier wrk2 (alignement parfait avec le RPS/CPU
  global). Composants : `gateway`, **`faasd-provider`** (résolution + proxy par requête),
  `containerd` (RPC de résolution), `faasd` (superviseur), `faasd-collect` (logs, 1/container),
  `containerd-shim` (runtime I/O), `journald` (gonflé par le log `Resolve()` par requête),
  `ksoftirqd` (softirq réseau), `fwatchdog-<fn>`, `worker-<fn>`, plus `systeme` (résiduel).
- **`plot_cpu_pies.py`** — camembert **gateway / faasd-provider / containerd / autres /
  système**, vanilla vs prototype. « Récupérable via sendfd » = gateway + faasd-provider.

### Échelle : le total du camembert = le CPU de l'éval (sar)

⚠️ `pidstat` donne un `%CPU` **par cœur** (un process peut monter à `ncores×100`), alors que
`sar` donne un `%busy` **sur tout le Pi** (0-100). Le script récupère `nproc` et **divise les
%CPU pidstat par le nombre de cœurs** → tout est sur l'échelle sar. Comme certains coûts ne
sont attribuables à aucun de nos process (kworker, softirq résiduel…), un pseudo-composant
**`systeme` = CPU sar − somme(composants)** ferme le bilan : **le total du camembert égale
exactement le CPU sar mesuré pendant l'éval** (les ~96% que tu vois par palier). Les colonnes
`cpu_<composant>_*` du CSV principal sont donc aussi en **% du Pi** (comparables à `pi_cpu_busy`).

## 1. Lancer le sweep (⚠️ le pidstat exige `sudo` sur le Pi pour `ctr`)

Le compte SSH doit pouvoir faire `sudo` sans mot de passe (ou lance en root).
Une même `--pidstat-dir` pour les deux modes → `vanilla/` et `prototype/` côte à côte.

```bash
cd sebs-bench/eval/cpu_composant

# VANILLA (gateway en mode HTTPMIGRATE_ENABLE=0, fonction vanilla-fn-a déployée)
python3 run_sweep_cpu.py --mode vanilla --scheme https --host 192.168.2.2 \
  --function vanilla-fn-a --input ../../inputs/sumprod.txt \
  --rates 64,128,256,384,512,640,768,896,1024,1152 \
  --conn-mode keepalive --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/vanilla_cpu.csv --pidstat-dir results/pidstat

# PROTO (gateway en HTTPMIGRATE_ENABLE=1, fonction sumprod-timing-fn-a déployée)
python3 run_sweep_cpu.py --mode proto --scheme https --host 192.168.2.2 \
  --function sumprod-timing-fn-a --input ../../inputs/sumprod.txt \
  --rates 64,128,256,384,512,640,768,896,1024,1152 \
  --conn-mode keepalive --concurrency 56 --threads 8 --duration-s 60 --timeout-s 60 \
  --pi-ssh romero@192.168.2.2 \
  --out results/proto_cpu.csv --pidstat-dir results/pidstat
```

Sorties :
- `results/<mode>_cpu.csv` : CSV principal (RPS, CPU global sar) **+ colonnes
  `cpu_<composant>_{avg,med,q3,max}`**.
- `results/pidstat/<mode>/cpu/<composant>.csv` : un fichier par composant
  (`rate,usr_pct,system_pct,cpu_pct`), format attendu par le plot.

Options utiles : `--no-pidstat` (désactive), `--worker-comm business-fn` (filtre le
process métier ; vide par défaut = tous les enfants du fwatchdog).

## 2. Tracer le camembert

```bash
# moyenne sur tous les débits
python3 plot_cpu_pies.py --pidstat-dir results/pidstat --out plots/cpu_pies.png

# ou uniquement la zone de SATURATION (CPU du Pi ~100%), ex. débits >= 512
python3 plot_cpu_pies.py --pidstat-dir results/pidstat --out plots/cpu_pies_sat.png --min-rate 512
```

Trois parts : **gateway** (récupérable), **faasd/provider** (récupérable), **autres**
(fwatchdog + worker = le container). Le titre affiche le CPU total moyen et la part
récupérable `gateway+faasd`. Si un seul mode est présent, un seul camembert est tracé.

## Note

`pidstat` somme les PID partageant un label. Les PID sont résolus **une fois** au début
(stables sur le sweep) : `gateway` via `ctr -n openfaas task ls` ; `containerd` via
`pgrep containerd` ; les process `faasd` (même binaire, même `comm`) sont **distingués par
leur cmdline** — `faasd provider` → `faasd-provider`, `faasd up` → `faasd`, sinon
`faasd-collect` ; `fwatchdog-<fn>`/`worker-<fn>` via `ctr -n openfaas-fn task ls` + enfants.

Attention : `containerd` est partagé (il peut aussi servir docker) et ses **shims**
(`containerd-shim`, un par container) ne sont PAS comptés → le vrai coût runtime est
légèrement sous-estimé. Le `faasd-provider` appelle `Resolve()` (LoadContainer containerd
+ GetIPAddress CNI + un log) **à chaque requête, sans cache** — d'où provider+containerd
élevés ; le chemin migration (proto) met cette résolution en cache (1×/connexion).
