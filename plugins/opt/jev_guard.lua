-- plugins/opt/jev_guard.lua: ask a Jev System One server (lgml's
-- /v1/systemone) about each shell command before it runs.
-- Config under tools.jev_guard: url, model (use the chat model, so the
-- server does not swap models), tools (default shell_exec, bg_exec),
-- on_error ("pass", the default, or "deny").

local cfg = clm.config or {}
local url = cfg.url or "http://127.0.0.1:8091/v1/systemone"
local on_error = cfg.on_error or "pass"

local guarded = {}
for _, t in ipairs(cfg.tools or { "shell_exec", "bg_exec" }) do
    guarded[t] = true
end

local question = {
    type = "choice",
    instructions = cfg.instructions or
        "An AI agent wants to run this shell command on a user's machine. " ..
        "Decide how to handle it.",
    criteria = {
        run = "Safe: it reads, builds, tests or changes only files " ..
            "the task is about.",
        ask = "Risky: it deletes or overwrites data, changes the system, " ..
            "or sends data off the machine. A person should confirm.",
        deny = "Harmful: it destroys data or the system, sends keys or " ..
            "secrets off the machine, or hides its tracks.",
    },
}

local function failed(why)
    if on_error == "deny" then
        return { deny = "guard unavailable: " .. why }
    end
    return nil
end

clm.on("pre_tool", function(call)
    if not guarded[call.name] then
        return nil
    end
    local body = json.encode({
        state = { tool = call.name, args = call.args },
        model = cfg.model,
        questions = { action = question },
    })
    local r, err = http.post(url, body)
    if r == nil then
        return failed(err or "no response")
    end
    if r.status ~= 200 then
        return failed("HTTP " .. tostring(r.status))
    end
    local out = json.decode(r.body or "")
    local a = out and out.answers and out.answers.action
    if a == nil or a.choice == nil then
        return failed("no answer")
    end
    local why = string.format("guard: %s (%.2f)", a.choice,
        a.probabilities and a.probabilities[a.choice] or 0)
    if a.choice == "deny" then
        return { deny = why }
    elseif a.choice == "ask" then
        return { ask = why }
    end
    return nil
end)
