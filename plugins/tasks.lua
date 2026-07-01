-- plugins/tasks.lua
-- A simple task list, held in memory for the current session.
--
-- Demonstrates a stateful plugin: several tools sharing plugin-local state.
-- Tasks live in this plugin's Lua state, so they persist for the life of the
-- clm process and are lost on exit. Real persistence would need a scoped
-- filesystem/storage capability, which the sandbox does not expose yet (the
-- sandbox has no io/os by design). See docs/plugin-trust-model.md.
--
-- All tools are marked no_prompt: they only mutate an in-memory list, with no
-- exec/network/filesystem effect, so they are safe to run unprompted.

local tasks = {}   -- array of { id = N, text = "...", done = bool }
local next_id = 1

-- Render the whole list as text the model can read back.
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

-- Find a task by id; returns the task and its array index, or nil.
local function find(id)
    for i, t in ipairs(tasks) do
        if t.id == id then
            return t, i
        end
    end
    return nil, nil
end

clm.tool_register("task_add", {
    description = "Add a task to the session task list. Returns the new task id.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = {
            text = { type = "string", description = "the task description" },
        },
        required = { "text" },
    },
    invoke = function(args, ctx)
        local text = args.text
        if not text or text == "" then
            ctx:fail("missing 'text' argument")
            return
        end
        local t = { id = next_id, text = text, done = false }
        next_id = next_id + 1
        tasks[#tasks + 1] = t
        ctx:complete(string.format("added task %d: %s", t.id, t.text))
    end,
})

clm.tool_register("task_list", {
    description = "List all tasks in the session task list, with their status.",
    no_prompt = true,
    params_schema = { type = "object", properties = {} },
    invoke = function(args, ctx)
        ctx:complete(render())
    end,
})

clm.tool_register("task_done", {
    description = "Mark a task complete by its id.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = {
            id = { type = "integer", description = "the task id to complete" },
        },
        required = { "id" },
    },
    invoke = function(args, ctx)
        local id = args.id
        if type(id) ~= "number" then
            ctx:fail("missing or invalid 'id' argument")
            return
        end
        local t = find(id)
        if not t then
            ctx:fail(string.format("no task with id %d", id))
            return
        end
        t.done = true
        ctx:complete(string.format("completed task %d: %s", t.id, t.text))
    end,
})

clm.tool_register("task_remove", {
    description = "Remove a task from the list by its id.",
    no_prompt = true,
    params_schema = {
        type = "object",
        properties = {
            id = { type = "integer", description = "the task id to remove" },
        },
        required = { "id" },
    },
    invoke = function(args, ctx)
        local id = args.id
        if type(id) ~= "number" then
            ctx:fail("missing or invalid 'id' argument")
            return
        end
        local t, idx = find(id)
        if not t then
            ctx:fail(string.format("no task with id %d", id))
            return
        end
        table.remove(tasks, idx)
        ctx:complete(string.format("removed task %d: %s", t.id, t.text))
    end,
})
