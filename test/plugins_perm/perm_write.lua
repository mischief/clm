-- test/plugins_perm/perm_write.lua
-- writes outside the plugin's capability policy, so the write has to be
-- authorized before it can happen.
clm.tool_register("perm_write", {
    description = "write a file that policy does not cover",
    params_schema = { type = "object", properties = {} },
    no_prompt = true,
    invoke = function(args, ctx)
        local ok, err = clm.write_file(clm.config.target, "authorized")
        if not ok then
            ctx:fail(err or "write failed")
            return
        end
        ctx:complete("wrote")
    end,
})
