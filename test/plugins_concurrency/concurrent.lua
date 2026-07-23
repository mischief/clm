clm.tool_register("concurrent_async", {
    description = "exercise concurrent lua async operations",
    params_schema = {
        type = "object",
        properties = {
            get_url = { type = "string" },
            post_url = { type = "string" },
        },
        required = { "get_url", "post_url" },
    },
    no_prompt = true,
    invoke = function(args, ctx)
        local started = 0
        local tasks = {
            clm.spawn(function()
                started = started + 1
                local response, err = http.get(args.get_url)
                if response == nil then
                    error(err)
                end
                return response.body
            end),
            clm.spawn(function()
                started = started + 1
                local response, err = http.post(args.post_url, "{}")
                if response == nil then
                    error(err)
                end
                return response.body
            end),
            clm.spawn(function()
                started = started + 1
                clm.sleep(0)
                return "slept"
            end),
            clm.spawn(function()
                started = started + 1
                return "multi", "value"
            end),
        }
        if started ~= 0 then
            ctx:fail("spawn ran a task before returning")
            return
        end
        if tasks[1]:result() ~= nil then
            ctx:fail("pending task unexpectedly had a result")
            return
        end
        local first, wait_err = clm.wait_any(tasks)
        if first == nil then
            ctx:fail(wait_err)
            return
        end
        local first_outcome = tasks[first]:result()
        if first_outcome == nil or not first_outcome.ok then
            ctx:fail("wait_any returned an unsuccessful task")
            return
        end
        local outcomes = clm.await_all(tasks)
        for i, outcome in ipairs(outcomes) do
            if not outcome.ok then
                ctx:fail("task " .. i .. " failed: " .. outcome.error)
                return
            end
        end
        ctx:complete(table.concat({
            outcomes[1].value,
            outcomes[2].value,
            outcomes[3].value,
            outcomes[4].value[1],
            outcomes[4].value[2],
        }, ":"))
    end,
})

clm.tool_register("concurrent_error", {
    description = "exercise independent task outcomes",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function(_, ctx)
        local tasks = {
            clm.spawn(function()
                clm.sleep(0)
                return "survived"
            end),
            clm.spawn(function()
                error("child boom")
            end),
        }
        local outcomes = clm.await_all(tasks)
        if not outcomes[1].ok or outcomes[1].value ~= "survived" then
            ctx:fail("await_all cancelled an independent sibling")
            return
        end
        if outcomes[2].ok or not outcomes[2].error:find("child boom", 1, true) then
            ctx:fail("await_all did not preserve the child error")
            return
        end
        ctx:complete(outcomes[1].value .. ":" .. outcomes[2].error)
    end,
})

clm.tool_register("concurrent_try_error", {
    description = "exercise fail-fast task cancellation",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function(_, ctx)
        local tasks = {
            clm.spawn(function()
                clm.sleep(30000)
                return "too late"
            end),
            clm.spawn(function()
                error("try boom")
            end),
        }
        local results, err = clm.try_all(tasks)
        if results ~= nil then
            ctx:fail("try_all unexpectedly succeeded")
            return
        end
        local cancelled = tasks[1]:result()
        if cancelled == nil or not cancelled.cancelled then
            ctx:fail("try_all did not cancel the unfinished sibling")
            return
        end
        ctx:complete(err .. ":cancelled")
    end,
})

clm.tool_register("wait_timeout_cancel", {
    description = "exercise wait timeout and explicit cancellation",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function(_, ctx)
        local task = clm.spawn(function()
            clm.sleep(30000)
            return "too late"
        end)
        local index, err = clm.wait_any({ task }, 0)
        if index ~= nil or err ~= "timeout" then
            ctx:fail("wait_any did not time out")
            return
        end
        if not task:cancel("test cancellation") then
            ctx:fail("task cancellation failed")
            return
        end
        if not task:cancel("ignored replacement") then
            ctx:fail("task cancellation was not idempotent")
            return
        end
        task:wait()
        local outcome = task:result()
        if outcome == nil or not outcome.cancelled or
            outcome.error ~= "test cancellation" then
            ctx:fail("cancelled task result is incorrect")
            return
        end
        ctx:complete("timeout:cancelled")
    end,
})

clm.tool_register("complete_with_live_task", {
    description = "exercise completion with a live task",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function(_, ctx)
        clm.spawn(function()
            clm.sleep(30000)
        end)
        ctx:complete("must not succeed")
    end,
})

clm.tool_register("complete_with_unobserved_error", {
    description = "exercise completion with an unobserved task error",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function(_, ctx)
        local task = clm.spawn(function()
            error("unobserved boom")
        end)
        task:wait()
        ctx:complete("must not succeed")
    end,
})

clm.tool_register("unawaited_task", {
    description = "exercise abandoned task cancellation",
    params_schema = { type = "object" },
    no_prompt = true,
    invoke = function()
        clm.spawn(function()
            while true do
                clm.sleep(30000)
            end
        end)
    end,
})
