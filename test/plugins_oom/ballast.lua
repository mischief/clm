-- Fills the plugin heap to its cap and keeps it referenced, giving back just
-- enough to register the tool. The invocation then runs out of memory in the
-- unprotected prologue, where only a panic handler can save the process.
ballast = {}
local n = 0

pcall(function()
    while true do
        n = n + 1
        ballast[n] = string.rep("x", 4096)
    end
end)

for i = 0, 3 do
    ballast[n - i] = nil
end

clm.tool_register("ballast", {
    description = "never gets far enough to run",
    params_schema = { type = "object", properties = {} },
    no_prompt = true,
    invoke = function(args, ctx)
        ctx:complete("ok")
    end,
})
