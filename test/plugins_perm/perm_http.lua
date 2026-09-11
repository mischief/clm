-- test/plugins_perm/perm_http.lua
-- reaches a host the policy does not cover, so egress has to be
-- authorized before the request is started.
clm.tool_register("perm_http", {
    description = "fetch a url outside the plugin's allowlist",
    params_schema = { type = "object", properties = {} },
    no_prompt = true,
    invoke = function(args, ctx)
        local response, err = http.get("http://perm.invalid/resource")
        if response == nil then
            ctx:fail(err or "no response")
            return
        end
        ctx:complete(response.body or "ok")
    end,
})
