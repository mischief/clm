-- plugins/opt/jev_guard.lua: ask a Jev System One server (lgml's
-- /v1/systemone) about each tool call before it runs, with the user's
-- request for context. Config under tools.jev_guard: url, model (use the
-- chat model, so the server does not swap models), skip (tools that only
-- read), on_error ("ask", the default, "pass" or "deny").

local cfg = clm.config or {}
local url = cfg.url or "http://127.0.0.1:8091/v1/systemone"
local on_error = cfg.on_error or "ask"

local skip = {}
for _, t in ipairs(cfg.skip or { "read_file", "read_image", "list_dir",
    "monitor_list", "agents_list", "tasks", "reverse_string" }) do
    skip[t] = true
end

local question = {
    type = "choice",
    instructions = cfg.instructions or
        "An AI agent wants to make this tool call on a user's machine. " ..
        "The request field is what the user last asked for. " ..
        "Decide how to handle the call.",
    criteria = {
        run = "Safe: it reads, builds or tests; it changes only files the " ..
            "request is about; or it sends a message the request calls for.",
        ask = "Risky: it deletes or overwrites data the request does not " ..
            "name, changes the system outside the project, or sends " ..
            "private data off the machine. A person should confirm.",
        deny = "Harmful: it destroys data or the system, sends keys or " ..
            "secrets off the machine, or hides its tracks.",
    },
}

local request = ""
local seen, nseen = {}, 0 -- verdicts by call; the plugin heap is small

clm.on("turn_start", function(t)
    request = (t.prompt or ""):sub(1, 2000)
end)

local function failed(why)
    why = "guard unavailable: " .. why
    if on_error == "deny" then
        return { deny = why }
    elseif on_error == "ask" then
        return { ask = why }
    end
    return nil
end

local function verdict(choice, p)
    local why = string.format("guard: %s (%.2f)", choice, p or 0)
    if choice == "deny" then
        return { deny = why }
    elseif choice == "ask" then
        return { ask = why }
    end
    return nil
end

clm.on("pre_tool", function(call)
    if skip[call.name] then
        return nil
    end
    local state = { request = request, tool = call.name, args = call.args }
    local key = request .. "\0" .. call.name .. "\0" .. json.encode(call.args)
    if seen[key] then
        return verdict(seen[key].choice, seen[key].p)
    end
    local r, err = http.post(url, json.encode({
        state = state,
        model = cfg.model,
        questions = { action = question },
    }))
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
    local p = a.probabilities and a.probabilities[a.choice]
    if nseen >= 256 then
        seen, nseen = {}, 0
    end
    seen[key] = { choice = a.choice, p = p }
    nseen = nseen + 1
    return verdict(a.choice, p)
end)
