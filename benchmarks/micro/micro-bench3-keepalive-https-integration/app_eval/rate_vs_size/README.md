# rate_vs_size — RPS max (sans erreur) par taille d'image

Pour **chaque taille d'image**, on balaye une liste de **débits** croissants et
on relève le **RPS max atteint sans aucune erreur**. On compare ensuite
**prototype** vs **vanilla** sur un même graphe (RPS max en fonction de la taille).

## Principe
- Cible : point d'entrée `objectrecognition` (déclenche toute la chaîne BeFaaS).
- Charge : wrk2 open-loop (`-c<concurrency> -R<rate>`), POST multipart de l'image.
- Pour chaque `(taille, débit)` : on lit le RPS atteint + les erreurs.
- Un palier est **« sans erreur »** si : 0 non-2xx, 0 socket connect/read/write,
  0 timeout, ET le RPS atteint ≥ `tolerance × débit_visé` (le serveur a suivi).
- **RPS max d'une taille** = le plus grand RPS atteint parmi les paliers sans erreur.
- Par défaut, le balayage d'une taille **s'arrête au 1er palier en échec**
  (les débits supérieurs échoueront aussi) — désactivable avec `--no-stop-on-error`.

## Structure
```
rate_vs_size/
├── run_rate_vs_size.py     # le sweep (taille × débit → RPS max sans erreur)
├── plot_rate_vs_size.py    # plot RPS max vs taille (proto vs vanilla)
├── client/post_image.lua   # POST multipart image (keep-alive)
├── results/                # CSV résumé + détaillé
└── plots/                  # figures PNG
```
Les images sont réutilisées depuis `../base_latence/images/` (`img-<KB>kb.jpg`)
— surchargeable avec `--images-dir`.

## Lancer (tous les paramètres sont modifiables)

```bash
cd .../app_eval/rate_vs_size

# Prototype HTTPS
python3 run_rate_vs_size.py --mode proto --scheme https --host 192.168.2.2 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 \
  --rates 2,4,6,8,10,12,16,20,30,40,50 \
  --concurrency 32 --threads 4 --duration-s 20 --timeout-s 30 --pause 3 \
  --out results/proto_https.csv

# Vanilla HTTPS (après avoir déployé vanilla)
python3 run_rate_vs_size.py --mode vanilla --scheme https --host 192.168.2.2 \
  --sizes 2,4,8,16,32,64,128,256,512,1024 \
  --rates 2,4,6,8,10,12,16,20,30,40,50 \
  --concurrency 32 --threads 4 --duration-s 20 --timeout-s 30 --pause 3 \
  --out results/vanilla_https.csv
```

| Paramètre | Rôle |
|---|---|
| `--mode proto\|vanilla` | mode déployé (label de sortie) |
| `--scheme http\|https` | 8080 / 8443 |
| `--host` / `--port` | IP du Pi / port override |
| `--function` | fonction d'entrée (def objectrecognition) |
| `--sizes` | tailles d'image testées (KB) |
| `--rates` | débits balayés pour chaque taille |
| `--images-dir` | dossier des images (def ../base_latence/images) |
| `--concurrency` | connexions wrk2 — **assez haut** pour atteindre le RPS visé |
| `--threads` / `--duration-s` / `--timeout-s` / `--pause` | threads / durée palier / timeout / pause |
| `--rps-tolerance` | seuil rps_atteint/débit_visé pour valider un palier (def 0.90) |
| `--no-stop-on-error` | balaye tous les débits même après un échec |
| `--out` / `--detail-out` | CSV résumé / CSV détaillé |

## Sorties
- `results/<mode>_<scheme>.csv` — **résumé** : 1 ligne/taille (`max_rps_no_error`, `rate_at_max`).
- `results/<mode>_<scheme>_detail.csv` — **détail** : 1 ligne/(taille,débit).

## Plot comparatif
```bash
python3 plot_rate_vs_size.py \
  results/proto_https.csv results/vanilla_https.csv \
  --output plots/max_rps_vs_size_https.png
```

## ⚠️ Pré-requis mesure fiable
- Le mode testé (proto/vanilla) doit être **déployé** sur le Pi.
- **faasd ne doit pas flapper** : s'il redémarre (check EULA + Internet du Pi
  instable), le gateway tombe par intervalles → `socket_connect_errors` → RPS faussé.
- `--concurrency` doit être ≥ `RPS_visé × latence` pour ne pas brider le débit
  côté client (sinon on mesure la limite du client, pas du serveur).
