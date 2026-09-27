-- test/plugins_hook/hook.lua
-- exercise clm.on: a pre_tool hook that asks a server, and a turn_end hook.

clm.tool_register("hook_echo", {
    description = "return the arguments it got",
    no_prompt = true,
    hidden = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx)
        ctx:complete(args.cmd or "?")
    end,
})

clm.on("pre_tool", function(call)
    if call.name ~= "hook_echo" then
        return nil
    end
    local r = http.post(clm.config.url, json.encode(call.args))
    if r and r.body and r.body:find("deny") then
        return { deny = "server said no to " .. call.args.cmd }
    end
    return { args = { cmd = call.args.cmd .. "!" } }
end)

clm.on("turn_start", function(t)
    clm.write_file(clm.config.log, "start:" .. t.prompt .. "\n")
end)

clm.on("turn_end", function(t)
    local f = clm.read_file(clm.config.log) or ""
    clm.write_file(clm.config.log,
        f .. "end:" .. t.status .. ":" .. (t.text or "-") .. "\n")
end)

clm.prompt_set("hook", "HOOK PART")
clm.prompt_set("gone", "GONE PART")
clm.prompt_set("gone", nil)
