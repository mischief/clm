local json = require("dkjson")
local dir = arg[0]:match("^(.*)/") or "."
local url = arg[1] or "http://127.0.0.1:8091/v1/systemone"
local model = arg[2] or "qwen3.8-27b"

local template
local hooks = {}
clm = {
    config = { url = url, model = model },
    on = function(event, fn) hooks[event] = fn end,
}
http = {
    post = function(_, body)
        template = json.decode(body)
        return nil, "captured"
    end,
}
_G.json = { encode = json.encode, decode = json.decode }
dofile(dir .. "/../../plugins/opt/jev_guard.lua")
hooks.turn_start({ prompt = "" })
hooks.pre_tool({ name = "shell_exec", args = { command = "true" } })
local base = template.questions.action

local cases = dofile(dir .. "/corpus.lua")
local questions = {}
for _, c in ipairs(cases) do
    questions[c.id] = {
        type = base.type,
        criteria = base.criteria,
        instructions = {
            task = base.instructions,
            request = c.request,
            tool = c.tool,
            args = c.args,
        },
    }
end

local body = json.encode({
    state = "Tool calls an AI agent wants to make, one per question.",
    model = model,
    questions = questions,
})
local tmp = os.tmpname()
local f = assert(io.open(tmp, "w"))
f:write(body)
f:close()
local p = io.popen("curl -s -m 600 --data-binary @" .. tmp .. " '" .. url .. "'")
local out = json.decode(p:read("a"))
p:close()
os.remove(tmp)
if out == nil or out.answers == nil then
    io.stderr:write("no answers from " .. url .. "\n")
    os.exit(2)
end

local pass, fail = 0, 0
for _, c in ipairs(cases) do
    local a = out.answers[c.id]
    local got = a and a.choice or "none"
    if (" " .. c.expect .. " "):find(" " .. got .. " ", 1, true) then
        pass = pass + 1
    else
        fail = fail + 1
        print(string.format("FAIL %-24s want %-9s got %-4s %.2f", c.id,
            c.expect, got, a and a.probabilities and a.probabilities[got] or 0))
    end
end

print(string.format("%d/%d passed", pass, pass + fail))
os.exit(fail == 0 and 0 or 1)
