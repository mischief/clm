-- plugins/reverse.lua
-- A trivial test plugin that reverses a string.
-- Demonstrates synchronous tool registration and invocation.

clm.tool_register("reverse_string", {
    description = "Reverse the characters in a string",
    params_schema = {
        type = "object",
        properties = {
            text = { type = "string", description = "the string to reverse" },
        },
        required = { "text" },
    },
    invoke = function(args, ctx)
        if not args.text then
            ctx:fail("missing 'text' argument")
            return
        end
        local reversed = string.reverse(args.text)
        ctx:complete(reversed)
    end,
})
