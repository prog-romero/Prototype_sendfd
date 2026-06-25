-- post_image.lua (rate_vs_size)
-- wrk2 : POST multipart/form-data d'une IMAGE, en keep-alive.
--
-- Pour mesurer le RPS soutenable : wrk2 maintient -c connexions et tire à -R
-- req/s (open-loop). Le keep-alive amortit le coût TCP/TLS côté CLIENT, mais
-- chaque requête objectrecognition déclenche 3 appels inter-fonctions (node-fetch
-- = nouvelles connexions au gateway), donc la migration TLS reste fortement
-- exercée à chaque requête.
--
-- Variables d'environnement :
--   WRK_IMAGE_PATH  — chemin de l'image à uploader (requis)
--   WRK_PATH        — chemin de la requête (def: /function/objectrecognition)

local image_path = os.getenv("WRK_IMAGE_PATH") or "images/img-8kb.jpg"
local path       = os.getenv("WRK_PATH")       or "/function/objectrecognition"

local fh = io.open(image_path, "rb")
if not fh then
  error("post_image.lua: impossible d'ouvrir l'image: " .. image_path)
end
local image_data = fh:read("*a")
fh:close()

local boundary = "----wrk2BeFaaSRateVsSizeBnd"
local body =
  "--" .. boundary .. "\r\n" ..
  'Content-Disposition: form-data; name="image"; filename="img.jpg"\r\n' ..
  "Content-Type: image/jpeg\r\n\r\n" ..
  image_data .. "\r\n" ..
  "--" .. boundary .. "--\r\n"

wrk.method  = "POST"
wrk.body    = body
wrk.headers["Content-Type"]   = "multipart/form-data; boundary=" .. boundary
wrk.headers["Content-Length"] = tostring(#body)

request = function()
  return wrk.format(nil, path)
end
