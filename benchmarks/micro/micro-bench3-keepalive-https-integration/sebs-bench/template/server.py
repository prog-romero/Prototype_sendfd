# server.py — HÔTE Python partagé par les 4 fonctions SeBS (équivalent du
# template node10-express-service utilisé pour BeFaaS, mais en Python).
#
# Rôle : exposer une fonction SeBS NON MODIFIÉE derrière un serveur HTTP que le
# watchdog (vanilla ou full-proxy) reverse-proxy sur 127.0.0.1:8085. Il N'ajoute
# AUCUNE logique métier : il ne fait qu'(1) héberger la fonction et (2) reproduire
# le WRAPPER DE MESURE de SeBS.
#
# Le wrapper de mesure est repris FIDÈLEMENT de SeBS
# (benchmarks/wrappers/openwhisk/python/__main__.py) : begin/end, results_time
# (µs), is_cold (marqueur /tmp/cold_run), et il propage le bloc `measurement`
# renvoyé par la fonction (download_time, compute_time, upload_size, ...).
#
# Différences avec OpenWhisk (sans impact sur la mesure) :
#   - l'event arrive dans le CORPS JSON de la requête HTTP (et non en argument
#     d'action OpenWhisk) ;
#   - request_id = uuid4 (faasd n'a pas de __OW_ACTIVATION_ID) ;
#   - les identifiants MinIO sont fournis par l'ENV du conteneur (déploiement),
#     donc déjà présents quand storage.py construit le client — on n'a pas à les
#     réinjecter par requête.
#
# Le chemin de l'URL est IGNORÉ par les fonctions SeBS (elles ne lisent que le
# corps), donc une route attrape-tout suffit : elle marche que faasd ait réécrit
# l'URL en `/` (vanilla) ou que le gateway ait rejoué `/function/<nom>` (proto).

import datetime
import os
import uuid

from flask import Flask, request, jsonify

# Import unique de la fonction SeBS (peut instancier le client MinIO à l'import
# via `client = storage.storage.get_instance()` -> l'ENV MinIO doit être posé).
from function import function as sebs_function

app = Flask(__name__)

COLD_MARKER = "/tmp/cold_run"


def _run(event):
    """Reproduit le wrapper SeBS autour de function.handler(event)."""
    begin = datetime.datetime.now()
    ret = sebs_function.handler(event)
    end = datetime.datetime.now()

    results_time = (end - begin) / datetime.timedelta(microseconds=1)

    # is_cold : vrai uniquement au tout premier appel de ce process (worker).
    is_cold = False
    if not os.path.exists(COLD_MARKER):
        is_cold = True
        open(COLD_MARKER, "a").close()

    log_data = {"result": ret["result"]}
    if "measurement" in ret:
        log_data["measurement"] = ret["measurement"]

    return {
        "begin": begin.strftime("%s.%f"),
        "end": end.strftime("%s.%f"),
        "request_id": str(uuid.uuid4()),
        "results_time": results_time,   # µs : temps serveur mesuré dans la fonction
        "is_cold": is_cold,
        "result": log_data,
    }


@app.route("/", defaults={"_path": ""}, methods=["POST", "GET"])
@app.route("/<path:_path>", methods=["POST", "GET"])
def handle(_path):
    # L'event = corps JSON (vide par défaut pour un simple probe de santé).
    event = request.get_json(silent=True) or {}
    try:
        return jsonify(_run(event))
    except Exception as e:  # noqa: BLE001 — on renvoie l'erreur comme SeBS le fait
        now = datetime.datetime.now()
        return jsonify({
            "begin": now.strftime("%s.%f"),
            "end": now.strftime("%s.%f"),
            "request_id": str(uuid.uuid4()),
            "results_time": 0,
            "result": "Error - invocation failed! Reason: {}".format(e),
        }), 500


if __name__ == "__main__":
    # Lancement direct (debug). En production c'est gunicorn qui sert `server:app`
    # (voir fprocess dans les Dockerfiles) pour exploiter les 4 cœurs du Pi.
    port = int(os.environ.get("http_port", "8085"))
    app.run(host="127.0.0.1", port=port)
