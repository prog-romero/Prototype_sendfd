#!/usr/bin/env python3
"""
average_csv.py — moyenne de plusieurs CSV de sweep (alignés par `rate`).

Prend N fichiers CSV (mêmes colonnes, produits par run_sebs_sweep.py) et écrit
UN CSV où, pour chaque `rate`, chaque colonne NUMÉRIQUE est la MOYENNE sur les
fichiers d'entrée. Les colonnes textuelles (timestamp, mode, scheme, function,
conn_mode, …) sont reprises telles quelles (première valeur rencontrée).

Une colonne `n_avg` indique sur combien de fichiers chaque `rate` a été moyenné
(utile si les fichiers n'ont pas exactement les mêmes paliers).

Usage :
  python3 average_csv.py f1.csv f2.csv [f3.csv ...] -o moyenne.csv
  python3 average_csv.py --key rate --decimals 3 f1.csv f2.csv -o out.csv
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import pandas as pd
except ImportError:
    print("ERREUR: pandas requis (pip3 install --break-system-packages pandas)", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    p = argparse.ArgumentParser(description="Moyenne de plusieurs CSV de sweep, alignés par `rate`.")
    p.add_argument("inputs", nargs="+", help="fichiers CSV à moyenner (au moins 2)")
    p.add_argument("-o", "--out", required=True, help="fichier CSV de sortie")
    p.add_argument("--key", default="rate", help="colonne d'alignement (défaut: rate)")
    p.add_argument("--decimals", type=int, default=3, help="arrondi des colonnes numériques (défaut: 3)")
    args = p.parse_args()

    if len(args.inputs) < 2:
        print("  [warn] un seul fichier fourni -> la 'moyenne' est ce fichier.", file=sys.stderr)

    # ── lecture ──────────────────────────────────────────────────────────────
    dfs = []
    for f in args.inputs:
        if not Path(f).is_file():
            print(f"ERREUR: fichier introuvable: {f}", file=sys.stderr)
            return 1
        dfs.append(pd.read_csv(f))

    # colonnes communes à TOUS les fichiers, dans l'ordre du premier
    common = [c for c in dfs[0].columns if all(c in d.columns for d in dfs)]
    if args.key not in common:
        print(f"ERREUR: la colonne clé '{args.key}' est absente d'au moins un fichier.", file=sys.stderr)
        return 1
    dfs = [d[common] for d in dfs]

    allrows = pd.concat(dfs, ignore_index=True)

    # ── numériques (moyenne) vs texte (première valeur) ──────────────────────
    num_cols = [c for c in common
                if c != args.key and pd.api.types.is_numeric_dtype(allrows[c])]
    txt_cols = [c for c in common if c != args.key and c not in num_cols]

    agg = {c: "mean" for c in num_cols}
    agg.update({c: "first" for c in txt_cols})

    grouped = allrows.groupby(args.key, as_index=False).agg(agg)
    counts = (allrows.groupby(args.key, as_index=False)
              .size().rename(columns={"size": "n_avg"}))
    out = grouped.merge(counts, on=args.key).sort_values(args.key)

    if num_cols:
        out[num_cols] = out[num_cols].round(args.decimals)

    # remet les colonnes dans l'ordre d'origine, + n_avg à la fin
    ordered = [c for c in common if c in out.columns] + ["n_avg"]
    out = out[ordered]

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    out.to_csv(args.out, index=False)
    print(f"[ok] moyenne de {len(dfs)} fichier(s) -> {args.out}  "
          f"({len(out)} paliers, {len(num_cols)} colonnes numériques moyennées)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
