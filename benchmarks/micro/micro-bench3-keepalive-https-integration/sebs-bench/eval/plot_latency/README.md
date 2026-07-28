# plot_latency — latence moyenne (avant saturation) vanilla vs prototype

`plot_latency.py` trace un **histogramme groupé** : une application par groupe,
deux barres (**vanilla** en orange, **prototype/sendfd** en bleu), et le **gain de
latence en vert** au-dessus de chaque paire. Ordonnée = latence moyenne (ms).

Pour chaque application il moyenne la colonne `lat_avg_ms` du CSV, **uniquement
sur les débits avant saturation** (au-delà, la latence explose à cause du CPU
saturé et fausserait la moyenne). Les latences `0.0` (mesure manquante) sont ignorées.

## Format des arguments — ⚠️ ordre des CSV

Une `--app` par application, avec **4 valeurs dans cet ordre** :

```
--app  NOM  COUPURE  CSV_VANILLA  CSV_PROTO
```

- **`CSV_VANILLA` d'ABORD, `CSV_PROTO` ENSUITE** (les inverser donne un gain négatif absurde).
- **`COUPURE`** = un nombre (le rate max à inclure) **ou** `auto` (détection automatique).

CSV attendus : colonnes `rate`, `rps`, `lat_avg_ms` (produits par `run_*_sweep.py`).

## Détection auto du débit de saturation (`auto`)

Avec `auto`, le cutoff est détecté sur le **CSV vanilla** : c'est le **dernier rate
où le serveur suit le débit** (`rps >= 0.95*rate`) **ET** où la latence n'a pas
explosé (`lat <= 3x la plus petite latence vue`). On coupe à la première saturation.
Exemples détectés : fonction-vide `192`, graph-pagerank `96`, dynamic-html `80`, Befaas `10`.

## Exemple (les 4 applications, trié par latence croissante)

```bash
cd sebs-bench/eval/plot_latency
DIR=../../../CSV-bench

python3 plot_latency.py --sort \
  --app fonction-vide  auto $DIR/fonction-vide/keepalive/vanilla_https_sumprod_keepalive.csv \
                             $DIR/fonction-vide/keepalive/proro_https_sumprod_keepalive.csv \
  --app dynamic-html   auto $DIR/dynamic-html/dynamic-keepalive/vanilla_https_fin.csv \
                             $DIR/dynamic-html/dynamic-keepalive/proto_https_fin.csv \
  --app graph-pagerank auto $DIR/graph-pagerank/keepalive-same-fn/vanilla_https_fin.csv \
                             $DIR/graph-pagerank/keepalive-same-fn/proto_https_fin.csv \
  --app Befaas         auto $DIR/Befaas/vanilla_https_objreco_32c_new.csv \
                             $DIR/Befaas/proto_https_objreco_32c_new.csv \
  --out latency_moy.png
```

Sortie console (vérifie les cutoffs et les gains) :
```
  fonction-vide   vanilla= 171.9 ms   proto=  22.4 ms   (-87%)   rates 0..auto=192
  dynamic-html    vanilla= 281.4 ms   proto= 110.2 ms   (-61%)   rates 0..auto=80
  graph-pagerank  vanilla= 451.1 ms   proto= 231.4 ms   (-49%)   rates 0..auto=96
  Befaas          vanilla=2530.0 ms   proto=2116.0 ms   (-16%)   rates 0..auto=10
```

## Options

| Option | Effet |
|---|---|
| `--sort` | ordonne les applications par latence **vanilla croissante** (la plus petite d'abord) |
| `--min-rate N` | ignore aussi les rate `< N` (utile pour sauter un warm-up bruité) |
| `--latency-col COL` | colonne de latence à moyenner (déf `lat_avg_ms`) |
| `--title "..."` | titre du graphe |
| `--out FICHIER.png` | image de sortie |

## Pièges fréquents (vus dans les commandes ratées)
- **Inverser vanilla/proto** → gain négatif (ex. `-526%`). Toujours VANILLA puis PROTO.
- **Coupure trop haute** (ex. dynamic-html `176`) → inclut la latence explosée (7970 ms) et
  gonfle la moyenne. Utilise `auto` ou le vrai point de saturation.
- **Typo de chemin** (`.cs` au lieu de `.csv`) → `[warn] introuvable` + application ignorée.
