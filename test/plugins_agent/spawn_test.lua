-- test/plugins_agent/spawn_test.lua
-- Exercises agent.new()/:submit()/:free() for test_lua_agent.c.

clm.tool_register("spawn_test", {
    description = "spawn a subagent and submit one prompt to it",
    no_prompt = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx)
        local child = agent.new({ max_iterations = 1 })
        local text, err = child:submit("say hi")
        child:free()
        if err then
            ctx:fail("child error: " .. tostring(err))
        else
            ctx:complete("child said: " .. tostring(text))
        end
    end,
})
