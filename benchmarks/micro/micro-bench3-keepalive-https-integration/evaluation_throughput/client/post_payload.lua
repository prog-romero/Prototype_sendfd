-- post_payload.lua
-- wrk Lua script: send POST requests with configurable payload size and target mode.
--
-- Environment variables (set before running wrk):
--   WRK_PAYLOAD_KB    — payload size in KB (default: 32)
--   WRK_TARGET_MODE   — "same" or "alternate" (default: "same")
--   WRK_SAME_TARGET   — function name when mode="same" (default: vanilla-fn-a)
--   WRK_FN_A          — function name A for alternating mode (default: vanilla-fn-a)
--   WRK_FN_B          — function name B for alternating mode (default: vanilla-fn-b)
--
-- Usage examples:
--   WRK_PAYLOAD_KB=32 WRK_TARGET_MODE=same WRK_SAME_TARGET=vanilla-fn-a \
--   wrk -t4 -c16 -d20s --latency \
--       -s post_payload.lua \
--       http://192.168.2.2:8080/function/vanilla-fn-a

local payload_kb    = tonumber(os.getenv("WRK_PAYLOAD_KB") or "32")
local payload_bytes = payload_kb * 1024
local target_mode   = os.getenv("WRK_TARGET_MODE") or "same"
local same_target   = os.getenv("WRK_SAME_TARGET") or "vanilla-fn-a"
local fn_a          = os.getenv("WRK_FN_A") or "vanilla-fn-a"
local fn_b          = os.getenv("WRK_FN_B") or "vanilla-fn-b"

-- Build the body once starting with two integers "10 20 " followed by padding to match payload size.
local prefix = "10 20 "
local pad_len = payload_bytes - #prefix
if pad_len < 0 then pad_len = 0 end
local body = prefix .. string.rep("X", pad_len)

-- Per-thread request counter used to alternate A/B in WRK_TARGET_MODE=alternate.
local req_no = 0

-- Set the global request template — wrk uses these for all requests.
wrk.method = "POST"
wrk.body   = body
wrk.headers["Content-Type"]   = "application/octet-stream"
wrk.headers["Content-Length"] = tostring(#body)

request = function()
	local path

	if target_mode == "alternate" then
		if (req_no % 2) == 0 then
			path = "/function/" .. fn_a
		else
			path = "/function/" .. fn_b
		end
		req_no = req_no + 1
	else
		path = "/function/" .. same_target
	end

	return wrk.format(nil, path)
end
