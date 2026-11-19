-- wrk script: send N pipelined requests per round trip (default 16).
-- usage: wrk ... -s bench/pipeline.lua URL -- 16
local depth = 16

init = function(args)
  depth = tonumber(args[1]) or depth
  local r = {}
  for i = 1, depth do
    r[i] = wrk.format(nil)
  end
  req = table.concat(r)
end

request = function()
  return req
end
