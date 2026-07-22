clm.tool_register("loop_after_http", {
    description = "loop after an http yield",
    params_schema = { type = "object", properties = {} },
    timeout_ms = 5,
    no_prompt = true,
    invoke = function(args, ctx)
        http.get("http://test.invalid/")
        while true do end
    end,
})

clm.tool_register("loop_after_sleep", {
    description = "loop after a sleep yield",
    params_schema = { type = "object", properties = {} },
    timeout_ms = 5,
    no_prompt = true,
    invoke = function(args, ctx)
        clm.sleep(0)
        while true do end
    end,
})
