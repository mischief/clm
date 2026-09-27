-- plugins/opt/cron.lua: scheduled prompts. A job fires as a message to the
-- agent: it starts a turn when the agent is idle, or joins the running one.
-- Jobs come from tools.cron.jobs in the config, and from the model through
-- cron_add. Model jobs and each job's last run live in tools.cron.store
-- (default $CLM_SCRATCH/cron.json, so per session).

local cfg = clm.config or {}
local MAX_JOBS = cfg.max_jobs or 16
local MIN_EVERY = cfg.min_every or 300
local MAX_WAIT = 3600

local scratch = clm.getenv("CLM_SCRATCH")
local store = cfg.store or (scratch and scratch .. "/cron.json")

local fields = {
    { lo = 0, hi = 59 }, -- minute
    { lo = 0, hi = 23 }, -- hour
    { lo = 1, hi = 31 }, -- day of month
    { lo = 1, hi = 12 }, -- month
    { lo = 0, hi = 7 },  -- day of week, 0 and 7 are Sunday
}

local function parse_field(s, f)
    local set, any = {}, s == "*"
    for part in s:gmatch("[^,]+") do
        local range, step = part:match("^([^/]+)/(%d+)$")
        range = range or part
        step = tonumber(step) or 1
        local a, b
        if range == "*" then
            a, b = f.lo, f.hi
        else
            a, b = range:match("^(%d+)-(%d+)$")
            a, b = tonumber(a), tonumber(b)
            if a == nil then
                a = tonumber(range)
                b = a
                if a ~= nil and part:find("/") then b = f.hi end
            end
        end
        if a == nil or b == nil or a < f.lo or b > f.hi or a > b or step < 1 then
            return nil
        end
        for v = a, b, step do set[v] = true end
    end
    return set, any
end

-- "m h dom mon dow" -> spec table, or nil and why.
local function parse(expr)
    local parts = {}
    for p in expr:gmatch("%S+") do parts[#parts + 1] = p end
    if #parts ~= 5 then
        return nil, "want 5 fields: minute hour day month weekday"
    end
    local spec = {}
    for i, f in ipairs(fields) do
        local set, any = parse_field(parts[i], f)
        if set == nil then
            return nil, "bad field " .. i .. ": " .. parts[i]
        end
        spec[i] = set
        spec["any" .. i] = any
    end
    if spec[5][7] then spec[5][0] = true end
    return spec
end

local function day_ok(spec, t)
    local dom, dow = spec[3][t.day], spec[5][t.wday]
    if spec.any3 or spec.any5 then
        return dom and dow
    end
    return dom or dow
end

-- The first minute after `after` that spec matches, within about a year.
local function next_match(spec, after)
    local t = clm.localtime(after - after % 60 + 60)
    for _ = 1, 20000 do
        if not spec[4][t.month] then
            t = clm.localtime(clm.mktime({ year = t.year, month = t.month + 1, day = 1 }))
        elseif not day_ok(spec, t) then
            t = clm.localtime(clm.mktime({ year = t.year, month = t.month, day = t.day + 1 }))
        elseif not spec[2][t.hour] then
            t = clm.localtime(clm.mktime({ year = t.year, month = t.month, day = t.day, hour = t.hour + 1 }))
        elseif not spec[1][t.min] then
            t = clm.localtime(clm.mktime({ year = t.year, month = t.month, day = t.day, hour = t.hour, min = t.min + 1 }))
        else
            return clm.mktime(t)
        end
    end
    return nil
end

local jobs = {}      -- by id
local order = {}     -- ids, in the order they were added
local last_run = {}  -- by id: when the job last fired
local seq = 0
local timer
local waiting = {}   -- ids fired since the last turn began or ended

local function when(t)
    local l = clm.localtime(t)
    return string.format("%04d-%02d-%02d %02d:%02d", l.year, l.month, l.day, l.hour, l.min)
end

local function next_due(job, now)
    if job.once then
        return job.once
    elseif job.every then
        return (last_run[job.id] or job.added or now) + job.every
    end
    return next_match(job.spec, last_run[job.id] or job.added or now)
end

local function save()
    if store == nil then return end
    local model = {}
    for _, id in ipairs(order) do
        local j = jobs[id]
        if j.model then
            model[#model + 1] = { id = j.id, at = j.at, every = j.every,
                once = j.once, prompt = j.prompt, added = j.added }
        end
    end
    clm.write_file(store, json.encode({ seq = seq, jobs = model, last = last_run }))
end

local function remove(id)
    if jobs[id] == nil then return false end
    jobs[id] = nil
    last_run[id] = nil
    for i, v in ipairs(order) do
        if v == id then table.remove(order, i) break end
    end
    return true
end

local arm

local function fire(job, now, missed)
    if waiting[job.id] then
        return
    end
    waiting[job.id] = true
    last_run[job.id] = now
    local note = missed and string.format(" (missed at %s)", when(missed)) or ""
    clm.notify(string.format("[cron %s] %s%s", job.id, job.prompt, note))
    if job.once then remove(job.id) end
end

local function tick()
    timer = nil
    local now = clm.time()
    for _, id in ipairs({ table.unpack(order) }) do
        local job = jobs[id]
        if job ~= nil then
            local due = next_due(job, now)
            if due ~= nil and due <= now then
                fire(job, now, due < now - 90 and due or nil)
            end
        end
    end
    save()
    arm()
end

arm = function()
    if timer ~= nil then timer:cancel() end
    local now, soonest = clm.time(), nil
    for _, id in ipairs(order) do
        local due = next_due(jobs[id], now)
        if due ~= nil and (soonest == nil or due < soonest) then soonest = due end
    end
    if soonest == nil then return end
    local wait = math.max(1, math.min(soonest - now, MAX_WAIT))
    timer = clm.after(wait * 1000, tick)
end

local function add(j)
    if j.at ~= nil then
        local spec, err = parse(j.at)
        if spec == nil then return nil, err end
        j.spec = spec
    elseif j.every ~= nil then
        if type(j.every) ~= "number" or j.every < 1 then return nil, "every must be seconds" end
    elseif j.once == nil then
        return nil, "want at, every or in"
    end
    if type(j.prompt) ~= "string" or j.prompt == "" then return nil, "want a prompt" end
    jobs[j.id] = j
    order[#order + 1] = j.id
    return j
end

local function load()
    local text = store and clm.read_file(store)
    local st = text and json.decode(text) or {}
    seq = st.seq or 0
    last_run = st.last or {}
    for i, j in ipairs(cfg.jobs or {}) do
        add({ id = j.name or ("c" .. i), at = j.at, every = j.every,
            prompt = j.prompt, added = clm.time() })
    end
    for _, j in ipairs(st.jobs or {}) do
        j.model = true
        add(j)
    end
end

clm.tool_register("cron_add", {
    description = "Schedule a prompt for yourself. Give exactly one of: " ..
        "at, a 5-field cron expression in local time (\"0 9 * * 1-5\"); " ..
        "every, a period in seconds; in, a delay in seconds for a one-time " ..
        "reminder. When it fires, the prompt arrives as a message.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = {
            prompt = { type = "string", description = "what to do when it fires" },
            at = { type = "string" },
            every = { type = "integer" },
            ["in"] = { type = "integer" },
        },
        required = { "prompt" },
    },
    invoke = function(args, ctx)
        local n = 0
        for _, id in ipairs(order) do
            if jobs[id].model then n = n + 1 end
        end
        if n >= MAX_JOBS then
            return ctx:fail("too many jobs; remove one with cron_remove")
        end
        if args.every ~= nil and args.every < MIN_EVERY then
            return ctx:fail("every must be at least " .. MIN_EVERY .. " seconds")
        end
        local now = clm.time()
        seq = seq + 1
        local j, err = add({ id = "m" .. seq, model = true, added = now,
            at = args.at, every = args.every, prompt = args.prompt,
            once = args["in"] and now + math.max(args["in"], 1) or nil })
        if j == nil then
            seq = seq - 1
            return ctx:fail(err)
        end
        save()
        arm()
        local due = next_due(j, now)
        ctx:complete(string.format("added %s; next at %s", j.id,
            due and when(due) or "never"))
    end,
})

clm.tool_register("cron_list", {
    description = "List scheduled prompts.",
    no_prompt = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(_, ctx)
        local now, lines = clm.time(), {}
        for _, id in ipairs(order) do
            local j = jobs[id]
            local due = next_due(j, now)
            lines[#lines + 1] = string.format("%s %s next %s: %s", id,
                j.at and ("at " .. j.at) or j.every and ("every " .. j.every .. "s")
                    or "once", due and when(due) or "never", j.prompt)
        end
        ctx:complete(#lines > 0 and table.concat(lines, "\n") or "no jobs")
    end,
})

clm.tool_register("cron_remove", {
    description = "Remove a scheduled prompt you added, by id.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = { id = { type = "string" } },
        required = { "id" },
    },
    invoke = function(args, ctx)
        local j = jobs[args.id]
        if j == nil then
            return ctx:fail("no job " .. tostring(args.id))
        elseif not j.model then
            return ctx:fail(args.id .. " comes from the config")
        end
        remove(args.id)
        save()
        arm()
        ctx:complete("removed " .. args.id)
    end,
})

local function settle()
    waiting = {}
end
clm.on("turn_start", settle)
clm.on("turn_end", settle)

load()
tick()

return { parse = parse, next_match = next_match }
