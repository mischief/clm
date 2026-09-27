local plugin = arg[1]
local fails, checks = 0, 0

local function check(cond, what)
    checks = checks + 1
    if not cond then
        fails = fails + 1
        print("FAIL " .. what)
    end
end

local function stamp(y, mo, d, h, mi)
    return os.time({ year = y, month = mo, day = d, hour = h, min = mi, sec = 0 })
end

local function env(opts)
    local e = { now = opts.now, notes = {}, timer = nil, tools = {}, hooks = {} }
    local stored = opts.stored
    e.clm = {
        config = opts.config or {},
        time = function() return e.now end,
        localtime = function(t)
            local d = os.date("*t", t or e.now)
            return { year = d.year, month = d.month, day = d.day, hour = d.hour,
                min = d.min, sec = d.sec, wday = d.wday - 1, isdst = d.isdst }
        end,
        mktime = function(t)
            return os.time({ year = t.year, month = t.month, day = t.day or 1,
                hour = t.hour or 0, min = t.min or 0, sec = t.sec or 0 })
        end,
        getenv = function(k) return k == "CLM_SCRATCH" and "/scratch" or nil end,
        read_file = function() return stored and "x" or nil end,
        write_file = function() end,
        notify = function(s)
            e.notes[#e.notes + 1] = s
            if e.hooks.turn_start then e.hooks.turn_start({ prompt = s }) end
        end,
        after = function(ms, fn)
            e.timer = { at = e.now + ms / 1000, fn = fn }
            return { cancel = function() e.timer = nil end }
        end,
        tool_register = function(name, def) e.tools[name] = def end,
        on = function(ev, fn) e.hooks[ev] = fn end,
    }
    e.json = {
        encode = function() return "" end,
        decode = function() return stored end,
    }
    function e.run_until(t)
        while e.timer ~= nil and e.timer.at <= t do
            local tm = e.timer
            e.timer = nil
            e.now = tm.at
            tm.fn()
        end
        e.now = t
    end
    function e.call(name, args)
        local out
        local ctx = {
            complete = function(_, s) out = { ok = true, text = s } end,
            fail = function(_, s) out = { ok = false, text = s } end,
        }
        e.tools[name].invoke(args, ctx)
        return out
    end
    _G.clm, _G.json = e.clm, e.json
    e.mod = dofile(plugin)
    return e
end

local e = env({ now = stamp(2026, 9, 26, 12, 0) })
local p = e.mod
check(p.parse("* * *") == nil, "three fields are rejected")
check(p.parse("61 * * * *") == nil, "minute 61 is rejected")
check(p.parse("*/15 9-17 * * 1-5") ~= nil, "a step and ranges parse")

local sat_noon = stamp(2026, 9, 26, 12, 0)
check(p.next_match(p.parse("0 9 * * 1-5"), sat_noon) == stamp(2026, 9, 28, 9, 0),
    "weekday 09:00 from Saturday noon is Monday 09:00")
check(p.next_match(p.parse("*/15 * * * *"), stamp(2026, 9, 26, 12, 7)) ==
    stamp(2026, 9, 26, 12, 15), "every quarter hour")
check(p.next_match(p.parse("0 0 1 * 1"), sat_noon) == stamp(2026, 9, 28, 0, 0),
    "day of month or day of week, when both are set")
check(p.next_match(p.parse("30 2 29 2 *"), sat_noon) == stamp(2028, 2, 29, 2, 30),
    "February 29 skips to the next leap year")

e = env({ now = stamp(2026, 9, 26, 12, 0),
    config = { jobs = { { name = "disk", every = 600, prompt = "check disk" } } } })
e.run_until(stamp(2026, 9, 26, 12, 25))
check(#e.notes == 2 and e.notes[1] == "[cron disk] check disk",
    "a config job fires every 600 s")

e = env({ now = stamp(2026, 9, 26, 12, 0),
    config = { jobs = { { name = "am", at = "0 9 * * *", prompt = "morning" } } },
    stored = { seq = 0, jobs = {}, last = { am = stamp(2026, 9, 24, 9, 0) } } })
check(#e.notes == 1 and e.notes[1]:find("missed at 2026%-09%-25 09:00") ~= nil,
    "a run missed while down fires once at start")

e = env({ now = stamp(2026, 9, 26, 12, 0) })
local r = e.call("cron_add", { prompt = "remind me", ["in"] = 120 })
check(r.ok and r.text:find("m1") ~= nil, "cron_add makes m1")
check(not e.call("cron_add", { prompt = "spam", every = 5 }).ok,
    "a short period is refused")
check(not e.call("cron_add", { prompt = "bad", at = "nope" }).ok,
    "a bad expression is refused")
e.run_until(stamp(2026, 9, 26, 12, 3))
check(#e.notes == 1 and e.notes[1] == "[cron m1] remind me", "a reminder fires once")
check(e.call("cron_list", {}).text == "no jobs", "a fired reminder is gone")
e.call("cron_add", { prompt = "x", every = 600 })
check(e.call("cron_remove", { id = "m2" }).ok, "cron_remove removes a job")

print(string.format("%d/%d checks passed", checks - fails, checks))
os.exit(fails == 0 and 0 or 1)
