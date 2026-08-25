-- plugins/web_search.lua
-- Web search via Tavily API (https://docs.tavily.com).
--
-- Requires a Tavily API key in ~/.config/clm/config.lua:
--
--   return {
--       tools = {
--           web_search = {
--               api_key = "tvly-YOUR-KEY-HERE",
--           },
--       },
--   }
--
-- Get a free key at https://tavily.com (1000 queries/month on free tier).

local api_key = clm.config.api_key or ""

clm.tool_register("web_search", {
    description = "Search the web and return titles, URLs, and content snippets.",
    params_schema = {
        type = "object",
        properties = {
            query = {
                type = "string",
                description = "search query",
            },
            max_results = {
                type = "integer",
                description = "max results to return (default 5, max 20)",
            },
            topic = {
                type = "string",
                description = "search category: general, news, or finance (default general)",
            },
        },
        required = { "query" },
    },
    timeout_ms = 15000,
    no_prompt = true,
    invoke = function(args, ctx)
        if not args.query or args.query == "" then
            ctx:fail("missing 'query' argument")
            return
        end

        local max = args.max_results or 5
        if max > 20 then max = 20 end

        local req = {
            query = args.query,
            max_results = max,
            api_key = api_key,
            include_answer = "basic",
            topic = args.topic or "general",
        }

        local body = json.encode(req)
        local resp, err = http.post("https://api.tavily.com/search", body)
        if err then
            ctx:fail("search request failed: " .. err)
            return
        end

        if resp.status ~= 200 then
            ctx:fail("search API returned status " .. tostring(resp.status))
            return
        end

        local data = json.decode(resp.body)
        if not data or not data.results then
            ctx:fail("unexpected response from search API")
            return
        end

        local lines = {}

        -- Include the AI-generated answer if present.
        if type(data.answer) == "string" and data.answer ~= "" then
            lines[#lines + 1] = "Answer: " .. data.answer
            lines[#lines + 1] = ""
        end

        for i, r in ipairs(data.results) do
            lines[#lines + 1] = string.format("%d. %s", i,
                r.title or "(no title)")
            lines[#lines + 1] = "   " .. (r.url or "")
            if type(r.content) == "string" and r.content ~= "" then
                lines[#lines + 1] = "   " .. r.content
            end
            lines[#lines + 1] = ""
        end

        if #lines == 0 then
            ctx:complete("No results found for: " .. args.query)
        else
            ctx:complete(table.concat(lines, "\n"))
        end
    end,
})
