#!/usr/bin/env python3
"""
seed-storage.py — prépare le stockage objet MinIO pour les fonctions SeBS qui en
ont besoin (thumbnailer + compression). À lancer UNE FOIS, SUR LE PI (MinIO est
publié sur le bridge 10.62.0.1:9000, non exposé au LAN).

Il crée le bucket et y dépose des inputs *représentatifs* aux emplacements
attendus par les events (voir ../inputs/*.json) :

  thumbnailer : <bucket>/thumbnailer/input/sample.jpg        (une image JPEG)
  compression : <bucket>/compression/input/dataset/<fichiers> (un jeu de fichiers)

Ce n'est PAS le code métier SeBS (qui reste verbatim) : c'est l'équivalent de la
fonction generate_input() des input.py SeBS, adapté pour semer MinIO directement.

Pré-requis sur le Pi :  pip3 install --break-system-packages minio pillow

Usage :
  python3 seed-storage.py
  python3 seed-storage.py --endpoint 10.62.0.1:9000 --bucket sebs-benchmarks \
     --comp-files 12 --comp-file-kb 64 --img-width 1600 --img-height 1200
"""
import argparse
import io
import os
import sys

try:
    from minio import Minio
except ImportError:
    print("ERREUR: pip3 install --break-system-packages minio pillow", file=sys.stderr)
    sys.exit(1)


def make_jpeg(width, height):
    from PIL import Image
    import random
    img = Image.new("RGB", (width, height))
    px = img.load()
    # bruit déterministe -> JPEG non trivial à décoder (charge CPU réaliste)
    random.seed(42)
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            c = (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            for dy in range(4):
                for dx in range(4):
                    if x + dx < width and y + dy < height:
                        px[x + dx, y + dy] = c
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=85)
    out.seek(0)
    return out


def main():
    ap = argparse.ArgumentParser(description="Seed MinIO pour les fonctions SeBS (thumbnailer + compression).")
    ap.add_argument("--endpoint", default="10.62.0.1:9000")
    ap.add_argument("--access-key", default="sebsadmin")
    ap.add_argument("--secret-key", default="sebssecret")
    ap.add_argument("--bucket", default="sebs-benchmarks")
    ap.add_argument("--img-width", type=int, default=16)
    ap.add_argument("--img-height", type=int, default=12)
    ap.add_argument("--comp-files", type=int, default=12, help="nb de fichiers du dataset compression")
    ap.add_argument("--comp-file-kb", type=int, default=64, help="taille de chaque fichier (KB)")
    args = ap.parse_args()

    client = Minio(args.endpoint, access_key=args.access_key,
                   secret_key=args.secret_key, secure=False)

    if not client.bucket_exists(args.bucket):
        client.make_bucket(args.bucket)
        print(f"[ok] bucket créé : {args.bucket}")
    else:
        print(f"[=] bucket déjà présent : {args.bucket}")

    # --- thumbnailer : une image JPEG ---
    jpg = make_jpeg(args.img_width, args.img_height)
    size = jpg.getbuffer().nbytes
    client.put_object(args.bucket, "thumbnailer/input/sample.jpg", jpg, size,
                      content_type="image/jpeg")
    print(f"[ok] thumbnailer/input/sample.jpg ({size} o, {args.img_width}x{args.img_height})")

    # --- compression : un dataset de fichiers sous dataset/ ---
    blob = os.urandom(args.comp_file_kb * 1024)
    for i in range(args.comp_files):
        key = f"compression/input/dataset/file_{i:03d}.bin"
        client.put_object(args.bucket, key, io.BytesIO(blob), len(blob))
    total_kb = args.comp_files * args.comp_file_kb
    print(f"[ok] compression/input/dataset/ : {args.comp_files} fichiers, ~{total_kb} KB")

    print("\n[done] seeding terminé. Vérif :")
    print(f"  mc ls myminio/{args.bucket}/  (ou via la console http://10.62.0.1:9001)")


if __name__ == "__main__":
    main()
