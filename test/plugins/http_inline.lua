-- test/plugins/http_inline.lua
-- exercise callbacks delivered by a synchronous http host.

clm.tool_register("http_inline", {
    description = "exercise synchronous http completion",
    no_prompt = true,
    hidden = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx)
        local resp, err = http.get("test://inline/success")
        if err then
            ctx:fail(err)
            return
        end

        local ignored, connect_err = http.post("test://inline/connect-error", "{}")
        if not connect_err then
            ctx:fail("expected synchronous connect failure")
            return
        end

        ctx:complete(resp.body .. ":" .. connect_err)
    end,
})
