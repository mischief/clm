-- test/plugins_exec/remove.lua
-- clm.tool_remove: a plugin removes its own tool, never another's.

clm.tool_register("gone", {
    description = "removed at load",
    invoke = function(args, ctx) ctx:complete("still here") end,
})
assert(clm.tool_remove("gone") == true)
assert(clm.tool_remove("gone") == false)
assert(clm.tool_remove("exec_tool") == false)
clm.tool_register("remove_ok", {
    description = "marks that the asserts above passed",
    invoke = function(args, ctx) ctx:complete("ok") end,
})
