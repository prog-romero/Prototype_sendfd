# server.py — hôte Python (Flask), calqué sur le template SeBS
# (sebs-bench/template/server.py). Il héberge function.handler derrière un
# serveur HTTP que le watchdog (vanilla ou full-proxy) reverse-proxy sur
# 127.0.0.1:8085, et reproduit le wrapper de mesure SeBS (results_time, is_cold).
#
# L'event est lu depuis le CORPS de la requête. Deux formats acceptés pour
# rester compatible avec le client d'éval :
#   - JSON  : {"a": 3, "b": 4}
#   - texte : "3 4"  (mêmes fichiers d'entrée que la fonction C : sumprod.txt…)
#
# Route attrape-tout : marche que faasd ait réécrit l'URL en `/` (vanilla) ou
# que le gateway ait rejoué `/function/<nom>` (proto).
import datetime
import os
import re
import uuid

from flask import Flask, request, jsonify

from function import handler as fn_handler

app = Flask(__name__)
COLD_MARKER = "/tmp/cold_run"


def _parse_event():
    j = request.get_json(silent=True)
    if isinstance(j, dict) and ("a" in j or "b" in j):
        return {"a": j.get("a", 0), "b": j.get("b", 0)}
    # Sinon : corps texte "a b …" -> les deux premiers entiers.
    nums = re.findall(r"-?\d+", request.get_data(as_text=True) or "")
    return {
        "a": int(nums[0]) if nums else 0,
        "b": int(nums[1]) if len(nums) > 1 else 0,
    }


@app.route("/", defaults={"_path": ""}, methods=["POST", "GET"])
@app.route("/<path:_path>", methods=["POST", "GET"])
def handle(_path):
    begin = datetime.datetime.now()
    ret = fn_handler(_parse_event())
    end = datetime.datetime.now()

    is_cold = not os.path.exists(COLD_MARKER)
    if is_cold:
        open(COLD_MARKER, "a").close()

    return jsonify({
        "begin": begin.strftime("%s.%f"),
        "end": end.strftime("%s.%f"),
        "request_id": str(uuid.uuid4()),
        "results_time": (end - begin) / datetime.timedelta(microseconds=1),
        "is_cold": is_cold,
        "result": ret,
    })


if __name__ == "__main__":
    # Lancement direct (debug). En production c'est gunicorn qui sert server:app
    # (voir start.sh / fprocess) pour exploiter les cœurs du Pi.
    app.run(host="127.0.0.1", port=int(os.environ.get("http_port", "8085")))
