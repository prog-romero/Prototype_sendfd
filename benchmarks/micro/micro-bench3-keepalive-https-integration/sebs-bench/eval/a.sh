#!/bin/bash
#
python3 run_sebs_sweep.py --mode proto --scheme https --host 192.168.2.2 --port 8443   --function dynamic-html --input ../inputs/dynamic-html.json   --rates 4   --concurrency 1 --threads 1 --duration-s 20 --timeout-s 60 --conn-mode keepalive   --pi-ssh romero@192.168.2.2 --out results/dynamic-html/proto_https_repro.csv



exit 0


