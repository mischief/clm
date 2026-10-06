local clm = require("clm")
local url = os.getenv("CLM_TEST_URL")
local fails, checks = 0, 0

local function check(cond, what)
    checks = checks + 1
    if not cond then
        fails = fails + 1
        print("FAIL " .. what)
    end
end

local function agent(opts)
    opts.url = opts.url or url
    opts.model = opts.model or "mock"
    return clm.agent(opts)
end

-- A turn outside clm.run drives the loop itself.
local a = agent{ stream = false }
local text, status = a:turn("hello")
check(status == 0 and #text > 0, "a turn outside clm.run returns the reply")
local h = a:history()
check(h[1].role == "system" and h[#h].role == "assistant" and h[#h].content == text,
    "history starts with the system prompt and ends with the reply")
check(a:state() == "complete", "state after a turn")
check(a:clear() and #a:history() == 1, "clear leaves only the system prompt")
a:close()

-- A Lua tool runs in a coroutine and may sleep.
local results = {}
local t = agent{
    on_tool = function(name, out, how) results[#results + 1] = how .. ":" .. out end,
}
t:tool{
    name = "shell_exec", description = "a fake shell", no_prompt = true,
    params = { type = "object", properties = { command = { type = "string" } } },
    invoke = function(args)
        clm.sleep(20)
        return "ran " .. args.command
    end,
}
clm.run(function()
    local reply, st = t:turn("shelltest")
    check(st == 0 and #reply > 0, "a tool turn finishes")
end)
check(results[1] == "ok:ran echo hi", "the Lua tool got its arguments and answered")

-- A pre_tool hook can deny, and the model sees why.
results = {}
t:on("pre_tool", function(call)
    if call.args.command == "echo hi" then
        return { deny = "not today" }
    end
end)
clm.run(function() t:turn("shelltest") end)
check(results[1] ~= nil and results[1]:find("not today") ~= nil,
    "pre_tool denies with its reason")

-- builtins = false drops the file tools; a list keeps the ones named.
for _, c in ipairs({ { false, "failed" }, { { "read_file" }, "ok" } }) do
    local how
    local b = agent{ builtins = c[1], permission = "allow",
        on_tool = function(_, _, h) how = h end }
    clm.run(function() b:turn("readtest") end)
    check(how == c[2], "builtins " .. tostring(c[1]) .. ": read_file " .. c[2])
    b:close()
end

-- Turn hooks see the prompt and the reply.
local seen = {}
t:on("turn_start", function(info) seen.prompt = info.prompt end)
t:on("turn_end", function(info) seen.text = info.text end)
clm.run(function() t:turn("say something") end)
check(seen.prompt == "say something" and seen.text ~= nil, "turn hooks fire")
t:close()

-- Two agents at once, inside one clm.run.
local b1, b2 = agent{}, agent{}
local done = 0
clm.run(function()
    clm.spawn(function()
        if b1:turn("one") then done = done + 1 end
    end)
    if b2:turn("two") then done = done + 1 end
    while done < 2 do clm.sleep(10) end
end)
check(done == 2, "two agents run at once")
b1:close()
b2:close()

-- Errors come back as nil and a message.
local bad = agent{ url = "http://127.0.0.1:1/v1", stream = false }
local r, err = bad:turn("hello")
check(r == nil and type(err) == "string", "a failed turn returns nil and why")
bad:close()

-- Closing an agent ends a turn that waits on it.
local c = agent{}
clm.run(function()
    local r2, err2
    clm.spawn(function() r2, err2 = c:turn("slowtest") end)
    clm.sleep(20)
    c:close()
    check(r2 == nil and err2 == "agent closed", "close ends a waiting turn")
end)

-- clm.post waits in its coroutine for the reply.
clm.run(function()
    local st, body = clm.post(url .. "/chat/completions",
        '{"model":"m","stream":false,"messages":[{"role":"user","content":"hi"}]}')
    check(st == 200 and body:find("choices") ~= nil, "clm.post returns status and body")
    local none, err = clm.post("http://127.0.0.1:1/x", "{}")
    check(none == nil and type(err) == "string", "a failed post returns nil and why")
end)

-- clm.exec runs a command without blocking the loop.
clm.run(function()
    local code, out, why = clm.exec("echo out; echo err >&2; exit 3")
    check(code == 3 and why == nil, "clm.exec returns the exit code")
    check(out:find("out") and out:find("err"), "clm.exec keeps stdout and stderr")
    code, out = clm.exec("pwd; cat", { cwd = "/tmp", stdin = "fed" })
    check(code == 0 and out == "/tmp\nfed", "clm.exec takes cwd and stdin")
    local ticks, done = 0, false
    clm.spawn(function()
        while not done do ticks = ticks + 1 clm.sleep(10) end
    end)
    code, out, why = clm.exec("sleep 5", { timeout_ms = 200 })
    done = true
    check(code == nil and why:find("timed out"), "clm.exec stops at its timeout")
    check(ticks >= 5, "other coroutines run while clm.exec waits")
    code, out = clm.exec("head -c 5000 /dev/zero | tr '\\0' x", { max = 100 })
    check(#out < 200 and out:find("more bytes"), "clm.exec keeps at most max bytes")
    code, out = clm.exec("sleep 30 & echo started")
    check(code == 0 and out:find("started"), "a job left in the background does not hang it")
end)
check(not pcall(clm.exec, "true"), "clm.exec outside clm.run is an error")

check(not pcall(clm.sleep, 1), "clm.sleep outside clm.run is an error")
clm.run(function()
    check(not pcall(clm.run, function() end), "clm.run does not nest")
    check(not pcall(clm.step), "clm.step does not run inside clm.run")
end)
check(not pcall(clm.run, function() error("boom") end), "clm.run raises")

-- An agent its own callbacks and tools capture is still collected.
local weak = setmetatable({}, { __mode = "v" })
do
    local x
    x = agent{ on_text = function() return x end }
    x:tool{ name = "selfref", invoke = function() return x:state() end }
    x:on("turn_end", function() return x end)
    weak[1] = x
end
collectgarbage("collect")
collectgarbage("collect")
check(weak[1] == nil, "an agent in a cycle with its callbacks is collected")

-- The registry does not grow with turns, tools, sleeps and posts.
local function registry_size()
    collectgarbage("collect")
    local n = 0
    for _ in pairs(debug.getregistry()) do n = n + 1 end
    return n
end
local before = registry_size()
for _ = 1, 5 do
    local r = agent{}
    r:tool{ name = "shell_exec", no_prompt = true,
        invoke = function() clm.sleep(1); return "x" end }
    clm.run(function()
        r:turn("shelltest")
        clm.post(url .. "/chat/completions", "{}")
        clm.spawn(function() clm.sleep(1) end)
        clm.sleep(5)
    end)
    r:close()
end
check(registry_size() <= before, "the registry does not grow")

print(string.format("%d/%d checks passed", checks - fails, checks))
if fails > 0 then os.exit(1) end
