-- test/plugins_guard/echo.lua: a tool for jev_guard to check.
clm.tool_register("guard_echo", {
    description = "echo", no_prompt = true, hidden = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx) ctx:complete("RAN " .. (args.cmd or "")) end,
})
