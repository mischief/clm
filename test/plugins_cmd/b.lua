-- A second plugin with a name a.lua already took: a.lua keeps it.
clm.command_register("dup", {
	help = "from b",
	run = function()
		return "dup from b"
	end,
})
local ok = pcall(clm.command_register, "Bad Name", {
	run = function() end,
})
clm.command_register("badname", {
	help = ok and "bad name accepted" or "bad name refused",
	run = function() end,
})
