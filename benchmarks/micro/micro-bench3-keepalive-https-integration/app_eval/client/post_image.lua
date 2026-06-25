-- post_image.lua
-- Script wrk/wrk2 : envoie un POST multipart/form-data avec une IMAGE vers une
-- fonction OpenFaaS (le point d'entrée objectrecognition du macro-bench BeFaaS IoT).
--
-- L'image est lue UNE fois au chargement du script et le corps multipart est
-- construit une fois ; toutes les requêtes réutilisent le même corps (comme
-- post_payload.lua le fait pour le payload octet-stream).
--
-- Variables d'environnement (à définir avant de lancer wrk) :
--   WRK_IMAGE_PATH  chemin du fichier image           (def: images/image-ambulance.jpg)
--   WRK_IMAGE_NAME  nom de fichier du champ multipart  (def: image.jpg)
--   WRK_IMAGE_CT    content-type de la partie          (def: image/jpeg)
--   WRK_PATH        chemin de la requête               (def: /function/objectrecognition)
--
-- Exemple :
--   WRK_IMAGE_PATH=images/image-ambulance.jpg WRK_PATH=/function/objectrecognition \
--   wrk -t4 -c100 -d20s -R200 --latency -s client/post_image.lua \
--       https://192.168.2.2:8443/function/objectrecognition

local image_path = os.getenv("WRK_IMAGE_PATH") or "images/image-ambulance.jpg"
local image_name = os.getenv("WRK_IMAGE_NAME") or "image.jpg"
local image_ct   = os.getenv("WRK_IMAGE_CT")   or "image/jpeg"
local path       = os.getenv("WRK_PATH")       or "/function/objectrecognition"

-- Lire les octets de l'image (mode binaire).
local fh = io.open(image_path, "rb")
if not fh then
  error("post_image.lua: impossible d'ouvrir l'image: " .. image_path)
end
local image_data = fh:read("*a")
fh:close()

-- Corps multipart/form-data avec un champ "image" (ce qu'attend @koa/multer
-- dans objectrecognition : upload.single('image')).
local boundary = "----wrk2BeFaaSBoundaryZ7Ma4YwKtR"
local body =
  "--" .. boundary .. "\r\n" ..
  'Content-Disposition: form-data; name="image"; filename="' .. image_name .. '"\r\n' ..
  "Content-Type: " .. image_ct .. "\r\n\r\n" ..
  image_data .. "\r\n" ..
  "--" .. boundary .. "--\r\n"

wrk.method = "POST"
wrk.body   = body
wrk.headers["Content-Type"]   = "multipart/form-data; boundary=" .. boundary
wrk.headers["Content-Length"] = tostring(#body)

request = function()
  return wrk.format(nil, path)
end
