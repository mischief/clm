local json = require("dkjson")
local dir = arg[0]:match("^(.*)/") or "."
local url = arg[1] or "http://127.0.0.1:8091/v1/systemone"
local model = arg[2] or "qwen3.8-27b"

local function post(u, body)
    local tmp = os.tmpname()
    local f = assert(io.open(tmp, "w"))
    f:write(body)
    f:close()
    local p = io.popen("curl -s -m 120 -w '\\n%{http_code}' " ..
        "--data-binary @" .. tmp .. " '" .. u .. "'")
    local out = p:read("a")
    p:close()
    os.remove(tmp)
    local resp, code = out:match("^(.*)\n(%d+)$")
    if code == nil then
        return nil, "curl failed"
    end
    return { status = tonumber(code), body = resp }
end

local hooks = {}
clm = {
    config = { url = url, model = model },
    on = function(event, fn) hooks[event] = fn end,
}
http = { post = post }
_G.json = { encode = json.encode, decode = json.decode }
dofile(dir .. "/../../plugins/opt/jev_guard.lua")

local cases = dofile(dir .. "/corpus.lua")
local pass, fail = 0, 0

for _, c in ipairs(cases) do
    hooks.turn_start({ prompt = c.request })
    local r = hooks.pre_tool({ name = c.tool, args = c.args })
    local got = "run"
    if r and r.deny then
        got = "deny"
    elseif r and r.ask then
        got = "ask"
    end
    local ok = (" " .. c.expect .. " "):find(" " .. got .. " ", 1, true)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print(string.format("FAIL %-24s want %-9s got %-4s %s", c.id,
            c.expect, got, r and (r.deny or r.ask) or ""))
    end
end

print(string.format("%d/%d passed", pass, pass + fail))
os.exit(fail == 0 and 0 or 1)
