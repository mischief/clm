-- test/plugins_proc/proc.lua
-- exercise clm.spawn, clm.after, clm.notify and clm.getenv at load time.

local lines = {}
clm.spawn({ "sh", "-c", "printf 'a\\nb\\r\\nc'; echo err >&2; exit 3" }, {
    on_line = function(line) lines[#lines + 1] = line end,
    on_exit = function(code, signal, stderr)
        clm.notify("spawn:" .. table.concat(lines, ",") .. ":" ..
            tostring(code) .. ":" .. tostring(signal) .. ":" .. tostring(stderr))
    end,
})

local sleeper = clm.spawn({ "sleep", "30" }, {
    on_exit = function(code, signal)
        clm.notify("killed:" .. tostring(code) .. ":" .. tostring(signal))
    end,
})
sleeper:kill("KILL")

clm.after(10, function() clm.notify("after:" .. tostring(sleeper:running())) end)
local t = clm.after(5, function() clm.notify("cancelled timer fired") end)
t:cancel()

local ok, err = pcall(clm.spawn, { "/nonexistent/clm-test" }, {})
clm.notify("env:" .. tostring(clm.getenv("CLM_TEST_ENV")) ..
    " missing:" .. tostring(not ok and err ~= nil))
