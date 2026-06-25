-- post_image_nokeep.lua
-- wrk2 script: POST multipart/form-data image with Connection: close.
--
-- With -c1 and Connection: close, wrk2 opens a NEW TCP+TLS connection for every
-- request. The latency wrk2 reports is therefore the full end-to-end client
-- latency of a fresh connection:
--   TCP SYN/ACK + TLS handshake + image upload + server processing
--   (whole objectrecognition chain: jimp + 3 migrated hops + Redis) + response.
--
-- This is exactly what `curl -w %{time_total}` measures for one request.
--
-- Environment variables:
--   WRK_IMAGE_PATH  — path to the image file to upload (required)
--   WRK_FUNCTION    — target function name (default: objectrecognition)

local image_path = os.getenv("WRK_IMAGE_PATH") or "images/img-8kb.jpg"
local fn_name    = os.getenv("WRK_FUNCTION")   or "objectrecognition"

local fh = io.open(image_path, "rb")
if not fh then
  error("post_image_nokeep.lua: cannot open image: " .. image_path)
end
local image_data = fh:read("*a")
fh:close()

-- multipart/form-data with a single field "image" (what @koa/multer expects:
-- upload.single('image') in objectrecognition).
local boundary = "----wrk2BeFaaSBaseLatenceBnd"
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
wrk.headers["Connection"]     = "close"   -- force a new TCP+TLS session per request

request = function()
  return wrk.format(nil, "/function/" .. fn_name)
end
