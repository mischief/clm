-- Slash commands for test_lua_plugin and the TUI tests.
clm.command_register("hello", {
	args = "[name]",
	help = "say hello",
	run = function(arg)
		return "hello " .. (arg ~= "" and arg or "world")
	end,
})
clm.command_register("boom", {
	help = "always fails",
	run = function()
		error("kaboom")
	end,
})
clm.command_register("quiet", {
	run = function() end,
})
clm.command_register("dup", {
	help = "from a",
	run = function()
		return "dup from a"
	end,
})
clm.command_register("gone", {
	run = function()
		return "still here"
	end,
})
clm.command_remove("gone")
-- These two read clm.config.url, which test_lua_plugin sets.
clm.command_register("fetch", {
	help = "get a page",
	run = function()
		local r, err = http.get(clm.config.url)
		if not r then
			error(err)
		end
		return r.body
	end,
})
clm.command_register("slow", {
	timeout_ms = 300,
	run = function()
		local r = http.get(clm.config.url)
		return r and r.body
	end,
})
