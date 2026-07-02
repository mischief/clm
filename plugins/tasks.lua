-- plugins/tasks.lua
-- Session task list as a single tool with an action verb.
--
-- One tool with actions (add, list, done, remove) instead of four separate
-- tools. Saves ~600 tokens per turn in schema overhead.

local tasks = {}
local next_id = 1

local function render()
    if #tasks == 0 then
        return "no tasks."
    end
    local lines = {}
    for _, t in ipairs(tasks) do
        lines[#lines + 1] = string.format(
            "[%s] %d. %s", t.done and "x" or " ", t.id, t.text)
    end
    return table.concat(lines, "\n")
end

local function find(id)
    for i, t in ipairs(tasks) do
        if t.id == id then
            return t, i
        end
    end
    return nil, nil
end

clm.tool_register("tasks", {
    description = "Manage the session task list. "
        .. "Actions: add, list, done, remove.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = {
            action = {
                type = "string",
                description = "one of: add, list, done, remove",
            },
            text = {
                type = "string",
                description = "task description (required for add)",
            },
            id = {
                type = "integer",
                description = "task id (required for done and remove)",
            },
        },
        required = { "action" },
    },
    invoke = function(args, ctx)
        local action = args.action

        if action == "add" then
            local text = args.text
            if not text or text == "" then
                ctx:fail("'add' requires a 'text' argument")
                return
            end
            local t = { id = next_id, text = text, done = false }
            next_id = next_id + 1
            tasks[#tasks + 1] = t
            ctx:complete(string.format("added task %d: %s", t.id, t.text))

        elseif action == "list" then
            ctx:complete(render())

        elseif action == "done" then
            local id = args.id
            if type(id) ~= "number" then
                ctx:fail("'done' requires an 'id' argument")
                return
            end
            local t = find(id)
            if not t then
                ctx:fail(string.format("no task with id %d", id))
                return
            end
            t.done = true
            ctx:complete(string.format("completed task %d: %s", t.id, t.text))

        elseif action == "remove" then
            local id = args.id
            if type(id) ~= "number" then
                ctx:fail("'remove' requires an 'id' argument")
                return
            end
            local t, idx = find(id)
            if not t then
                ctx:fail(string.format("no task with id %d", id))
                return
            end
            table.remove(tasks, idx)
            ctx:complete(string.format("removed task %d: %s", t.id, t.text))

        else
            ctx:fail("unknown action '" .. tostring(action)
                .. "'; use add, list, done, or remove")
        end
    end,
})
