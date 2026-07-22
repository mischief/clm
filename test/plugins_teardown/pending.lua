clm.tool_register("pending_http", {
    description = "wait for an http response",
    params_schema = { type = "object", properties = {} },
    no_prompt = true,
    invoke = function(args, ctx)
        local response, err = http.get("http://pending.invalid/request")
        if response == nil then
            ctx:fail(err)
            return
        end
        ctx:complete(response.body or "ok")
    end,
})

clm.tool_register("pending_sleep", {
    description = "wait for a sleep timer",
    params_schema = { type = "object", properties = {} },
    no_prompt = true,
    invoke = function(args, ctx)
        clm.sleep(30000)
        ctx:complete("awake")
    end,
})
