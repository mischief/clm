-- plugins/edit_file.lua
-- A str_replace-style file editing tool.
-- The model specifies an exact string to find and its replacement.

clm.tool_register("edit_file", {
    description = "Edit a file by replacing an exact string with new content. "
        .. "The old_str must match exactly (including whitespace and indentation). "
        .. "By default old_str must match exactly once; pass replace_all=true to "
        .. "replace every occurrence instead (e.g. renaming a symbol, or a "
        .. "mechanical single-character substitution across a whole file) rather "
        .. "than retyping surrounding text by hand, which risks transcription "
        .. "errors. Use this for surgical edits; use write_file for creating new files.",
    params_schema = {
        type = "object",
        properties = {
            path = {
                type = "string",
                description = "path to the file to edit",
            },
            old_str = {
                type = "string",
                description = "exact string to find in the file (must match "
                    .. "uniquely unless replace_all is true)",
            },
            new_str = {
                type = "string",
                description = "replacement string (empty string to delete)",
            },
            replace_all = {
                type = "boolean",
                description = "replace every non-overlapping occurrence of "
                    .. "old_str instead of requiring exactly one match "
                    .. "(default false)",
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

        -- Count non-overlapping occurrences (matches the replacement pass
        -- below, so the failure message and the eventual "replaced N"
        -- message always agree even for a self-overlapping old_str).
        local count = 0
        local pos = 1
        while true do
            local i = string.find(content, args.old_str, pos, true)
            if not i then break end
            count = count + 1
            pos = i + #args.old_str
        end

        if count == 0 then
            ctx:fail("old_str not found in " .. args.path)
            return
        end
        if count > 1 and not args.replace_all then
            ctx:fail("old_str matches " .. tostring(count)
                .. " times; must be unique (pass replace_all=true to replace "
                .. "all occurrences, or add more surrounding context to "
                .. "old_str to target just one)")
            return
        end

        -- Perform the replacement(s), left to right, non-overlapping.
        local parts = {}
        local n = 0
        pos = 1
        while true do
            local i = string.find(content, args.old_str, pos, true)
            if not i then break end
            parts[#parts + 1] = string.sub(content, pos, i - 1)
            parts[#parts + 1] = args.new_str
            pos = i + #args.old_str
            n = n + 1
            if not args.replace_all then break end
        end
        parts[#parts + 1] = string.sub(content, pos)
        local new_content = table.concat(parts)

        local ok, werr = clm.write_file(args.path, new_content)
        if not ok then
            ctx:fail("cannot write '" .. args.path .. "': " .. (werr or "unknown"))
            return
        end

        ctx:complete("ok: replaced " .. tostring(n) .. " occurrence(s) of "
            .. tostring(#args.old_str) .. " bytes with "
            .. tostring(#args.new_str) .. " bytes each in " .. args.path)
    end,
})
