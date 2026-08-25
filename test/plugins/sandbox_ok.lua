-- test/plugins/sandbox_ok.lua
-- Load-time sandbox assertions. If any unsafe global is reachable, this
-- raises at load and the plugin fails to load -- which the C test detects as
-- a sandbox escape. A clean sandbox loads this successfully and registers
-- the marker tool below.
--
-- This runs at file scope (load phase), so it exercises the sandbox as seen
-- by plugin top-level code.

assert(os == nil, "os must not be exposed")
assert(io == nil, "io must not be exposed")
assert(require == nil, "require must not be exposed")
assert(load == nil, "load must not be exposed")
assert(dofile == nil, "dofile must not be exposed")
assert(loadfile == nil, "loadfile must not be exposed")
assert(package == nil, "package must not be exposed")
assert(collectgarbage == nil, "collectgarbage must not be exposed")

-- Safe libraries that SHOULD be present.
assert(type(string) == "table", "string must be available")
assert(type(table) == "table", "table must be available")
assert(type(math) == "table", "math must be available")
assert(type(json) == "table", "json module must be available")
assert(type(clm) == "table", "clm module must be available")

-- Object keys serialize in sorted order: a tool schema built from a Lua table
-- rides in the prompt prefix a provider caches, and Lua's own table order
-- varies per process.
assert(json.encode({ b = 1, a = 2, c = 3 }) == '{"a":2,"b":1,"c":3}',
    "json.encode must sort object keys")

-- An empty table sorts nothing: the key array is never allocated, and
-- handing NULL to qsort is undefined even for zero elements.
assert(json.encode({}) == '{}', "json.encode must handle an empty table")
assert(json.encode({ outer = {} }) == '{"outer":{}}',
    "json.encode must handle a nested empty table")

-- Register a marker tool so the C test can confirm this plugin loaded fully.
clm.tool_register("sandbox_ok_marker", {
    description = "marker: the sandbox self-test plugin loaded cleanly",
    no_prompt = true,
    hidden = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx) ctx:complete("ok") end,
})
