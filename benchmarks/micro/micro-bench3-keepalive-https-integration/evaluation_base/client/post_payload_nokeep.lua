-- post_payload_nokeep.lua
-- wrk2 script: POST with Connection: close.
--
-- With -c 1 and Connection: close, wrk2 opens a NEW TCP+TLS connection for
-- every request.  The latency wrk2 reports therefore includes:
--   TCP SYN/ACK  +  TLS handshake  +  HTTP request/response round-trip.
-- This is the full end-to-end client-side latency for a fresh connection.
--
-- Environment variables:
--   WRK_PAYLOAD_KB  — payload size in KB (default: 1)
--   WRK_FUNCTION    — target function name (default: sumprod-timing-fn-a)

local payload_kb = tonumber(os.getenv("WRK_PAYLOAD_KB") or "1")
local fn_name    = os.getenv("WRK_FUNCTION") or "sumprod-timing-fn-a"

-- Body: "10 20 " followed by padding to reach exactly payload_kb * 1024 bytes.
local prefix  = "10 20 "
local pad_len = payload_kb * 1024 - #prefix
if pad_len < 0 then pad_len = 0 end
local body = prefix .. string.rep("X", pad_len)

wrk.method  = "POST"
wrk.body    = body
wrk.headers["Content-Type"]   = "application/octet-stream"
wrk.headers["Content-Length"] = tostring(#body)
wrk.headers["Connection"]     = "close"   -- force new TCP+TLS per request

request = function()
    return wrk.format(nil, "/function/" .. fn_name)
end
