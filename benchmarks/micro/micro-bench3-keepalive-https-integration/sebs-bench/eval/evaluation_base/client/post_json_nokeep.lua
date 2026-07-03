-- post_json_nokeep.lua — latence de BASE : POST de l'event JSON SeBS avec
-- "Connection: close" -> wrk2 (-c1) ouvre une NOUVELLE connexion TCP+TLS à
-- chaque requête. La latence mesurée inclut donc :
--   TCP SYN/ACK + handshake TLS + aller-retour requête/réponse HTTP.
-- C'est la latence bout-en-bout d'une connexion fraîche, à vide (pas de charge).
--
-- Variables d'env :
--   WRK_BODY_FILE : fichier JSON de l'event (../inputs/<fn>.json)
--   WRK_PATH      : chemin de la requête (ex: /function/dynamic-html)
--   WRK_PERF_FILE : (optionnel) fichier où logger results_time (µs) de chaque 200.

local path      = os.getenv("WRK_PATH") or "/"
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

wrk.method  = "POST"
wrk.body    = body
wrk.headers["Content-Type"]   = "application/json"
wrk.headers["Content-Length"] = tostring(#body)
wrk.headers["Connection"]     = "close"   -- force une nouvelle connexion par requête

request = function()
  return wrk.format("POST", path, nil, body)
end

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
