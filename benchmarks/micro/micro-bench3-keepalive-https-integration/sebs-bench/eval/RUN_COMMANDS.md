# Commandes d'évaluation SeBS — Vanilla vs Prototype (à lancer À LA MAIN)

Toutes les commandes se lancent depuis :
```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/eval
```

Chaque application a **son propre dossier** dans `results/`, contenant les CSV
proto ET vanilla (sweep + perf-cost, http + https). `--out` crée le dossier tout
seul. Structure finale :

```
results/
  dynamic-html/
    proto_http.csv        proto_https.csv
    vanilla_http.csv      vanilla_https.csv
    perfcost_proto_http.csv    perfcost_proto_https.csv
    perfcost_vanilla_http.csv  perfcost_vanilla_https.csv
  graph-pagerank/   (idem)
  thumbnailer/      (idem)
  compression/      (idem)
```

> ⚠️ Vanilla et proto ont les MÊMES noms de fonction → on ne peut pas déployer
> les deux en même temps. Ordre : (Phase A) proto déjà déployé → lancer tous les
> évals proto ; (Phase B) rebuild+deploy vanilla ; (Phase C) lancer tous les
> évals vanilla ; (Phase D) plots.

---

## PHASE A — PROTO (déjà déployé et vérifié)

### dynamic-html — proto
```bash
python3 run_sebs_sweep.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function dynamic-html --input ../inputs/dynamic-html.json --rates 2,4,8,16,32,64,128,256 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/dynamic-html/proto_http.csv

python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html --input ../inputs/dynamic-html.json --rates 2,4,8,16,32,64,128,256 --concurrency 32 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/dynamic-html/proto_https.csv

python3 run_perfcost.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function dynamic-html --input ../inputs/dynamic-html.json --requests 100 --warmup 5 --timeout 120 --out results/dynamic-html/perfcost_proto_http.csv

python3 run_perfcost.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html --input ../inputs/dynamic-html.json --requests 100 --warmup 5 --timeout 120 --out results/dynamic-html/perfcost_proto_https.csv
```

### graph-pagerank — proto
```bash
python3 run_sebs_sweep.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../inputs/graph-pagerank.json --rates 2,4,8,16,32,64,128,256 --concurrency 32 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/graph-pagerank/proto_http.csv

python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../inputs/graph-pagerank.json --rates 2,4,8,16,32,64,128,256 --concurrency 32 --threads 4 --duration-s 60 --timeout-s 60 --pause 5 --pi-ssh romero@192.168.2.2 --out results/graph-pagerank/proto_https_1.csv
python3 run_perfcost.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../inputs/graph-pagerank.json --requests 100 --warmup 5 --timeout 120 --out results/graph-pagerank/perfcost_proto_http.csv

python3 run_perfcost.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../inputs/graph-pagerank.json --requests 100 --warmup 5 --timeout 120 --out results/graph-pagerank/perfcost_proto_https.csv
```

### thumbnailer — proto  (MinIO seedé requis)
```bash
python3 run_sebs_sweep.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function thumbnailer --input ../inputs/thumbnailer.json --rates 2,4,8,16,32,64,128,256 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/thumbnailer/proto_http.csv

python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer --input ../inputs/thumbnailer.json --rates 2,4,8,16,32,64,128,256 --concurrency 32 --threads 4 --duration-s 60 --timeout-s 60 --pause 5 --pi-ssh romero@192.168.2.2 --out results/thumbnailer/proto_https_1.csv

python3 run_perfcost.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function thumbnailer --input ../inputs/thumbnailer.json --requests 100 --warmup 5 --timeout 120 --out results/thumbnailer/perfcost_proto_http.csv

python3 run_perfcost.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer --input ../inputs/thumbnailer.json --requests 100 --warmup 5 --timeout 120 --out results/thumbnailer/perfcost_proto_https.csv
```

### compression — proto  (MinIO seedé requis)
python3 deploy/seed-storage.py    
```bash
python3 run_sebs_sweep.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function compression --input ../inputs/compression.json --rates 2,4,8,16,32,64,128,256 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/compression/proto_http.csv

python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function compression --input ../inputs/compression.json --rates 2,4,8,16,32,64,128,256 --concurrency 32 --threads 8 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/compression/proto_https.csv

python3 run_perfcost.py --mode proto --scheme http --host 192.168.2.2 --port 8080 --function compression --input ../inputs/compression.json --requests 100 --warmup 5 --timeout 120 --out results/compression/perfcost_proto_http.csv

python3 run_perfcost.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function compression --input ../inputs/compression.json --requests 100 --warmup 5 --timeout 120 --out results/compression/perfcost_proto_https.csv
```

---

## PHASE B — Basculer sur VANILLA

```bash
# a) rebuild + push vanilla (depuis la RACINE du repo)
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd
./benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/build-all.sh vanilla

# b) sur le Pi : retirer proto, PURGER le cache image (sinon faasd garde :latest), déployer vanilla
ssh romero@192.168.2.2
GW=http://127.0.0.1:8080
faas-cli remove dynamic-html thumbnailer compression graph-pagerank --gateway $GW
for fn in dynamic-html thumbnailer compression graph-pagerank; do
  sudo ctr -n openfaas images rm docker.io/romerosdd/sebs-vanilla-$fn:latest 2>/dev/null || true
done
cd ~/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/deploy
./deploy-all.sh vanilla
faas-cli list --gateway $GW      # les 4 Ready
exit

# c) revenir dans eval/
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/eval
```

---

## PHASE C — VANILLA

### dynamic-html — vanilla
```bash
python3 run_sebs_sweep.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function dynamic-html --input ../inputs/dynamic-html.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/dynamic-html/vanilla_http.csv

python3 run_sebs_sweep.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html --input ../inputs/dynamic-html.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/dynamic-html/vanilla_https.csv

python3 run_perfcost.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function dynamic-html --input ../inputs/dynamic-html.json --requests 100 --warmup 5 --timeout 120 --out results/dynamic-html/perfcost_vanilla_http.csv

python3 run_perfcost.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html --input ../inputs/dynamic-html.json --requests 100 --warmup 5 --timeout 120 --out results/dynamic-html/perfcost_vanilla_https.csv
```

### graph-pagerank — vanilla
```bash
python3 run_sebs_sweep.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../inputs/graph-pagerank.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/graph-pagerank/vanilla_http.csv

python3 run_sebs_sweep.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../inputs/graph-pagerank.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/graph-pagerank/vanilla_https.csv

python3 run_perfcost.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../inputs/graph-pagerank.json --requests 100 --warmup 5 --timeout 120 --out results/graph-pagerank/perfcost_vanilla_http.csv

python3 run_perfcost.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../inputs/graph-pagerank.json --requests 100 --warmup 5 --timeout 120 --out results/graph-pagerank/perfcost_vanilla_https.csv
```

### thumbnailer — vanilla  (MinIO seedé requis)
```bash
python3 run_sebs_sweep.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function thumbnailer --input ../inputs/thumbnailer.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/thumbnailer/vanilla_http.csv

python3 run_sebs_sweep.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer --input ../inputs/thumbnailer.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/thumbnailer/vanilla_https.csv

python3 run_perfcost.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function thumbnailer --input ../inputs/thumbnailer.json --requests 100 --warmup 5 --timeout 120 --out results/thumbnailer/perfcost_vanilla_http.csv

python3 run_perfcost.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer --input ../inputs/thumbnailer.json --requests 100 --warmup 5 --timeout 120 --out results/thumbnailer/perfcost_vanilla_https.csv
```

### compression — vanilla  (MinIO seedé requis)
```bash
python3 run_sebs_sweep.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function compression --input ../inputs/compression.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/compression/vanilla_http.csv

python3 run_sebs_sweep.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function compression --input ../inputs/compression.json --rates 5,10,20,40,60,80 --concurrency 16 --threads 4 --duration-s 20 --timeout-s 30 --pause 5 --pi-ssh romero@192.168.2.2 --out results/compression/vanilla_https.csv

python3 run_perfcost.py --mode vanilla --scheme http --host 192.168.2.2 --port 8080 --function compression --input ../inputs/compression.json --requests 100 --warmup 5 --timeout 120 --out results/compression/perfcost_vanilla_http.csv

python3 run_perfcost.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function compression --input ../inputs/compression.json --requests 100 --warmup 5 --timeout 120 --out results/compression/perfcost_vanilla_https.csv
```

---

## PHASE D — Plots (Vanilla vs Proto, une fois les deux modes mesurés)

```bash
# dynamic-html
python3 ../../app_eval/compare_two_csv_plots.py results/dynamic-html/vanilla_http.csv  results/dynamic-html/proto_http.csv  --label-a Vanilla --label-b Prototype --prefix dynamic-html_http  --out-dir results/dynamic-html/plots_http
python3 ../../app_eval/compare_two_csv_plots.py results/dynamic-html/vanilla_https.csv results/dynamic-html/proto_https.csv --label-a Vanilla --label-b Prototype --prefix dynamic-html_https --out-dir results/dynamic-html/plots_https

# graph-pagerank
python3 ../../app_eval/compare_two_csv_plots.py results/graph-pagerank/vanilla_http.csv  results/graph-pagerank/proto_http.csv  --label-a Vanilla --label-b Prototype --prefix graph-pagerank_http  --out-dir results/graph-pagerank/plots_http
python3 ../../app_eval/compare_two_csv_plots.py results/graph-pagerank/vanilla_https.csv results/graph-pagerank/proto_https.csv --label-a Vanilla --label-b Prototype --prefix graph-pagerank_https --out-dir results/graph-pagerank/plots_https

# thumbnailer
python3 ../../app_eval/compare_two_csv_plots.py results/thumbnailer/vanilla_http.csv  results/thumbnailer/proto_http.csv  --label-a Vanilla --label-b Prototype --prefix thumbnailer_http  --out-dir results/thumbnailer/plots_http
python3 ../../app_eval/compare_two_csv_plots.py results/thumbnailer/vanilla_https.csv results/thumbnailer/proto_https.csv --label-a Vanilla --label-b Prototype --prefix thumbnailer_https --out-dir results/thumbnailer/plots_https

# compression
python3 ../../app_eval/compare_two_csv_plots.py results/compression/vanilla_http.csv  results/compression/proto_http.csv  --label-a Vanilla --label-b Prototype --prefix compression_http  --out-dir results/compression/plots_http
python3 ../../app_eval/compare_two_csv_plots.py results/compression/vanilla_https.csv results/compression/proto_https.csv --label-a Vanilla --label-b Prototype --prefix compression_https --out-dir results/compression/plots_https
```

---

## NOTE — perf-cost désormais INTÉGRÉ au sweep

Depuis la refonte, `run_sebs_sweep.py` remplit lui-même, par palier :
`client_ms_{avg,p50,p99}`, `server_ms_{avg,p50,p99}`, `overhead_ms_{avg,p50,p99}`
(sur TOUTES les requêtes du palier), plus le CPU (`sar -u`, 0-100 %) et le réseau
(`sar -n DEV` eth0, `net_kb_s_{avg,max}` = rx+tx). **Prérequis : `sar` (sysstat)
sur le Pi.** Les commandes `run_perfcost.py` des Phases A/C restent FACULTATIVES :
utiles seulement pour l'overhead EXACT par requête (percentile des différences).

---

## PHASE E — Latence de base (connexion neuve par requête, à vide)

À lancer depuis le sous-dossier `evaluation_base/` :
```bash
cd ~/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/eval/evaluation_base
```
Sortie rangée par app (`results/<app>/base_<mode>_<scheme>.csv`, proto ET vanilla côte à côte).

### PROTO (déployé) — http + https
```bash
python3 run_eval.py --mode proto --scheme http  --host 192.168.2.2 --port 8080 --function dynamic-html   --input ../../inputs/dynamic-html.json   --requests 50 --rate 2 --timeout-s 30 --out results/dynamic-html/base_proto_http.csv
python3 run_eval.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html   --input ../../inputs/dynamic-html.json   --requests 50 --rate 2 --timeout-s 30 --out results/dynamic-html/base_proto_https.csv

python3 run_eval.py --mode proto --scheme http  --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../../inputs/graph-pagerank.json --requests 50 --rate 2 --timeout-s 30 --out results/graph-pagerank/base_proto_http.csv
python3 run_eval.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../../inputs/graph-pagerank.json --requests 50 --rate 2 --timeout-s 30 --out results/graph-pagerank/base_proto_https.csv

python3 run_eval.py --mode proto --scheme http  --host 192.168.2.2 --port 8080 --function thumbnailer   --input ../../inputs/thumbnailer.json   --requests 50 --rate 2 --timeout-s 30 --out results/thumbnailer/base_proto_http.csv
python3 run_eval.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer   --input ../../inputs/thumbnailer.json   --requests 50 --rate 2 --timeout-s 30 --out results/thumbnailer/base_proto_https.csv

python3 run_eval.py --mode proto --scheme http  --host 192.168.2.2 --port 8080 --function compression   --input ../../inputs/compression.json   --requests 50 --rate 2 --timeout-s 30 --out results/compression/base_proto_http.csv
python3 run_eval.py --mode proto --scheme https --host 192.168.2.2 --port 8443 --function compression   --input ../../inputs/compression.json   --requests 50 --rate 2 --timeout-s 30 --out results/compression/base_proto_https.csv
```

### VANILLA (après Phase B : rebuild + deploy vanilla) — http + https
```bash
python3 run_eval.py --mode vanilla --scheme http  --host 192.168.2.2 --port 8080 --function dynamic-html   --input ../../inputs/dynamic-html.json   --requests 50 --rate 2 --timeout-s 30 --out results/dynamic-html/base_vanilla_http.csv
python3 run_eval.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function dynamic-html   --input ../../inputs/dynamic-html.json   --requests 50 --rate 2 --timeout-s 30 --out results/dynamic-html/base_vanilla_https.csv

python3 run_eval.py --mode vanilla --scheme http  --host 192.168.2.2 --port 8080 --function graph-pagerank --input ../../inputs/graph-pagerank.json --requests 50 --rate 2 --timeout-s 30 --out results/graph-pagerank/base_vanilla_http.csv
python3 run_eval.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function graph-pagerank --input ../../inputs/graph-pagerank.json --requests 50 --rate 2 --timeout-s 30 --out results/graph-pagerank/base_vanilla_https.csv

python3 run_eval.py --mode vanilla --scheme http  --host 192.168.2.2 --port 8080 --function thumbnailer   --input ../../inputs/thumbnailer.json   --requests 50 --rate 2 --timeout-s 30 --out results/thumbnailer/base_vanilla_http.csv
python3 run_eval.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function thumbnailer   --input ../../inputs/thumbnailer.json   --requests 50 --rate 2 --timeout-s 30 --out results/thumbnailer/base_vanilla_https.csv

python3 run_eval.py --mode vanilla --scheme http  --host 192.168.2.2 --port 8080 --function compression   --input ../../inputs/compression.json   --requests 50 --rate 2 --timeout-s 30 --out results/compression/base_vanilla_http.csv
python3 run_eval.py --mode vanilla --scheme https --host 192.168.2.2 --port 8443 --function compression   --input ../../inputs/compression.json   --requests 50 --rate 2 --timeout-s 30 --out results/compression/base_vanilla_https.csv
```
