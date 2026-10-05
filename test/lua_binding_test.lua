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

check(not pcall(clm.sleep, 1), "clm.sleep outside clm.run is an error")
check(not pcall(clm.run, function() error("boom") end), "clm.run raises")

print(string.format("%d/%d checks passed", checks - fails, checks))
if fails > 0 then os.exit(1) end
