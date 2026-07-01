-- plugins/edit_file.lua
-- A str_replace-style file editing tool.
-- The model specifies an exact string to find and its replacement.

clm.tool_register("edit_file", {
    description = "Edit a file by replacing an exact string with new content. "
        .. "The old_str must match exactly (including whitespace and indentation). "
        .. "Use this for surgical edits; use write_file for creating new files.",
    params_schema = {
        type = "object",
        properties = {
            path = {
                type = "string",
                description = "path to the file to edit",
            },
            old_str = {
                type = "string",
                description = "exact string to find in the file (must match uniquely)",
            },
            new_str = {
                type = "string",
                description = "replacement string (empty string to delete)",
            },
        },
        required = { "path", "old_str", "new_str" },
    },
    invoke = function(args, ctx)
        if not args.path or args.path == "" then
            ctx:fail("missing 'path'")
            return
        end
        if not args.old_str or args.old_str == "" then
            ctx:fail("missing 'old_str' (use write_file to create new files)")
            return
        end
        if args.new_str == nil then
            ctx:fail("missing 'new_str'")
            return
        end

        local content, err = clm.read_file(args.path)
        if not content then
            ctx:fail("cannot read '" .. args.path .. "': " .. (err or "unknown"))
            return
        end

        -- Count occurrences to ensure unique match.
        local count = 0
        local pos = 1
        while true do
            local i = string.find(content, args.old_str, pos, true)
            if not i then break end
            count = count + 1
            pos = i + 1
        end

        if count == 0 then
            ctx:fail("old_str not found in " .. args.path)
            return
        end
        if count > 1 then
            ctx:fail("old_str matches " .. tostring(count)
                .. " times; must be unique")
            return
        end

        -- Perform the replacement.
        local i = string.find(content, args.old_str, 1, true)
        local new_content = string.sub(content, 1, i - 1)
            .. args.new_str
            .. string.sub(content, i + #args.old_str)

        local ok, werr = clm.write_file(args.path, new_content)
        if not ok then
            ctx:fail("cannot write '" .. args.path .. "': " .. (werr or "unknown"))
            return
        end

        ctx:complete("ok: replaced " .. tostring(#args.old_str) .. " bytes with "
            .. tostring(#args.new_str) .. " bytes in " .. args.path)
    end,
})
