-- test/plugins_exec/exec.lua
-- exercise clm.exec from a tool call.

clm.tool_register("exec_tool", {
    description = "run a child and report its result",
    no_prompt = true,
    hidden = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx)
        local r = clm.exec({ "sh", "-c", "cat; echo out2; echo e >&2; exit 5" },
            { stdin = "in\n" })
        ctx:complete(tostring(r.code) .. "|" .. r.stdout .. "|" .. r.stderr)
    end,
})
