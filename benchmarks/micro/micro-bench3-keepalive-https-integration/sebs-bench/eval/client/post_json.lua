-- post_json.lua — wrk2 : POST d'un corps JSON (l'event SeBS) vers la fonction.
--
-- Variables d'env :
--   WRK_BODY_FILE : chemin du fichier JSON à envoyer (l'event, ../inputs/<fn>.json)
--   WRK_PATH      : chemin de la requête (ex: /function/dynamic-html)
--   WRK_PERF_FILE : (optionnel) fichier où logger, pour CHAQUE réponse 200, le
--                   champ "results_time" (µs) du wrapper SeBS = server_ms.
--                   run_sebs_sweep.py agrège ensuite ce fichier (moy/p50/p99).
--
-- Le même corps est renvoyé à chaque requête (charge à débit constant).
-- Le hook response() nous permet de PARSER NOUS-MÊMES le corps renvoyé par wrk2
-- (wrk2 ne le fait pas) → on récupère le server_ms de toutes les requêtes du
-- palier sans lancer run_perfcost.py séparément.

local path = os.getenv("WRK_PATH") or "/"
local body_file = os.getenv("WRK_BODY_FILE")
local perf_path = os.getenv("WRK_PERF_FILE")

local body = "{}"
if body_file then
  local f = io.open(body_file, "rb")
  if f then
    body = f:read("*all")
    f:close()
  end
end

wrk.method = "POST"
wrk.body = body
wrk.headers["Content-Type"] = "application/json"

-- Mode NON keep-alive ("initial") : une nouvelle connexion par requête.
-- WRK_CONN_CLOSE=1 -> on envoie "Connection: close", le serveur ferme après
-- chaque réponse et wrk2 se reconnecte (donc handshake TLS + migration frais à
-- chaque requête). Non défini/0 -> keep-alive (comportement par défaut de wrk2).
if os.getenv("WRK_CONN_CLOSE") == "1" then
  wrk.headers["Connection"] = "close"
end

request = function()
  return wrk.format("POST", path, nil, body)
end

-- Fichier de collecte du server_ms, ouvert une fois par thread (append). Les
-- écritures de lignes courtes en O_APPEND sont atomiques sous Linux -> les
-- threads wrk2 peuvent écrire dans le même fichier sans se corrompre.
-- Ligne-bufferisé : write + flush par réponse (fiable même à faible débit où un
-- thread ne voit que quelques réponses). Le coût est négligeable (<= rps
-- écritures/s). La latence "-nan" occasionnelle de wrk2 (calibration
-- coordinated-omission perturbée quand sar tourne en parallèle) est gérée par un
-- RE-RUN du palier côté run_sebs_sweep.py, pas ici.
local perf_file = nil
if perf_path and perf_path ~= "" then
  perf_file = io.open(perf_path, "a")
end

response = function(status, headers, body)
  if perf_file and status == 200 then
    local rt = string.match(body, '"results_time":%s*([%d%.]+)')
    if rt then
      perf_file:write(rt, "\n")
      perf_file:flush()
    end
  end
end
