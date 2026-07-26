# function.py — logique métier (style SeBS, cf. graph-pagerank/function.py) :
# la fonction ne fait QUE la somme de deux nombres. Elle reçoit un `event`
# (dict) et renvoie {"result": ..., "measurement": {...}} comme les fonctions
# SeBS. Aucune I/O, aucun stockage : c'est volontairement trivial pour que le
# CPU mesuré soit celui du RUNTIME (Python/gunicorn), pas du calcul.
import datetime


def handler(event):
    a = event.get("a", 0)
    b = event.get("b", 0)

    begin = datetime.datetime.now()
    result = a + b
    end = datetime.datetime.now()

    compute_time = (end - begin) / datetime.timedelta(microseconds=1)
    return {
        "result": result,
        "measurement": {"compute_time": compute_time},
    }
