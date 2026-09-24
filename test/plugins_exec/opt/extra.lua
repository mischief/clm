-- test/plugins_exec/opt/extra.lua
-- an opt-in plugin: loads only when named.

clm.tool_register("opt_extra", {
    description = "opt-in plugin tool",
    no_prompt = true,
    hidden = true,
    invoke = function(args, ctx) ctx:complete(clm.config.greeting or "none") end,
})
