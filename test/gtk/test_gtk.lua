-- SPDX-License-Identifier: ISC
--
-- clm-gtk end to end: the real window on a private X server (Xvfb), driven
-- through AT-SPI with latspi, answered by the TUI tests' mock server. Run
-- by the tui_harness binary, which supplies `sys`, `mock` and `json`.
--
-- latspi is a separate project. The test finds it through $LATSPI_DIR (its
-- source tree, built in build/) or on the Lua paths, and skips without it.

local sys = require("sys")
local mock = require("mock")

local SKIP = 77

local function have(cmd)
	return os.execute("command -v " .. cmd .. " >/dev/null 2>&1") == true
end

for _, cmd in ipairs({ "Xvfb", "xdotool", "dbus-run-session" }) do
	if not have(cmd) then
		print("skip: needs " .. cmd)
		return SKIP
	end
end
local dir0 = os.getenv("LATSPI_DIR")
if dir0 and dir0 ~= "" then
	package.path = dir0 .. "/?.lua;" .. dir0 .. "/?/init.lua;" .. package.path
	package.cpath = dir0 .. "/build/?.so;" .. package.cpath
end
local okl, latspi = pcall(require, "latspi")
if not okl then
	print("skip: latspi not found (set LATSPI_DIR): " .. tostring(latspi))
	return SKIP
end
local BIN = os.getenv("CLM_GTK_BIN")
if not BIN then
	print("skip: CLM_GTK_BIN is not set")
	return SKIP
end

local failures = {}

local function check(cond, msg)
	print((cond and "ok  " or "FAIL") .. "  " .. msg)
	if not cond then
		failures[#failures + 1] = msg
	end
	return cond
end

local function read_file(p)
	local f = io.open(p)
	if not f then
		return nil
	end
	local s = f:read("*a")
	f:close()
	return s
end

local function write_file(p, s)
	local f = assert(io.open(p, "w"))
	f:write(s)
	f:close()
end

local function until_true(f, timeout)
	local deadline = sys.now() + timeout
	while true do
		local v = f()
		if v then
			return v
		end
		if sys.now() >= deadline then
			return nil
		end
		sys.sleep(0.1)
	end
end

local tmp = sys.mkdtemp("/tmp/clm-gtk-test-")
local pids = {}

local function cleanup()
	for _, pid in ipairs(pids) do
		os.execute("kill " .. pid .. " 2>/dev/null")
	end
	mock.request_log(nil)
	mock.stop()
	sys.rmtree(tmp)
end

-- Starts cmd in the background; returns its pid.
local function spawn(cmd)
	os.execute(cmd .. " & echo $! >" .. tmp .. "/pid")
	local pid = read_file(tmp .. "/pid"):match("%d+")
	pids[#pids + 1] = pid
	return tonumber(pid)
end

-- The window's own config, state and cache, with two models to choose from.
local url = mock.start():gsub("/chat/completions$", "")
for _, d in ipairs({ "config/clm/agents", "state", "cache" }) do
	os.execute("mkdir -p " .. tmp .. "/" .. d)
end
write_file(tmp .. "/config/clm/config.lua", ([[
return {
	model = "mock/mock-model",
	providers = {
		mock = {
			kind = "openai",
			url = %q,
			context_size = 100000,
			models = {
				["mock-model"] = {},
				["other-model"] = {},
			},
		},
	},
}
]]):format(url))

write_file(tmp .. "/config/clm/agents/other.lua", [=[return {
	system_prompt = "You are the other agent.",
	model = "mock/other-model",
}
]=])

local function run()
	spawn(("Xvfb -displayfd 3 -nolisten tcp 3>%s/display 2>/dev/null"):format(tmp))
	local display = until_true(function()
		return (read_file(tmp .. "/display") or ""):match("%d+")
	end, 10)
	if not check(display, "Xvfb started") then
		return
	end
	local reqlog = tmp .. "/requests.jsonl"
	mock.request_log(reqlog)

	local conn, err = latspi.connect()
	if not check(conn, "connect to the accessibility bus") then
		print("# " .. tostring(err))
		return
	end
	sys.mkdir(tmp .. "/runtime")
	local pid = spawn(("env DISPLAY=:%s GDK_BACKEND=x11 XDG_CONFIG_HOME=%s/config "
		.. "XDG_STATE_HOME=%s/state XDG_CACHE_HOME=%s/cache XDG_RUNTIME_DIR=%s/runtime "
		.. "PATH=%s/bin:$PATH %s --allow-all-tools >%s/gtk.log 2>&1"):format(display, tmp, tmp, tmp, tmp, tmp, BIN, tmp))
	local app = conn:wait_for(function()
		return conn:app({ pid = pid })
	end, 20)
	if not check(app, "the window appears on the accessibility bus") then
		return
	end

	local function node(q)
		return conn:wait_for(function()
			return app:find(q)
		end, 10)
	end
	local entry = node({ role = "text box", name = "Message" })
	local send = node({ role = "button", name = "Send" })
	local chats = node({ role = "toggle button", name = "Chats" })
	local eye = node({ role = "toggle button", name = "Show thinking" })
	local model = node({ role = "combo box", name = "Model" })
	local effort = node({ role = "combo box", name = "Effort" })
	check(entry and send and chats and eye and model and effort,
	    "every control has an accessible name")
	if not (entry and send and model and effort and chats) then
		print(app:dump(nil, { generic = false }))
		return
	end
	local press = latspi.xdotool(":" .. display)

	local function requests()
		local out = {}
		for ln in (read_file(reqlog) or ""):gmatch("[^\n]+") do
			out[#out + 1] = json.decode(ln)
		end
		return out
	end

	-- The chat's own rows. The chat list in the sidebar repeats a chat's
	-- first message as a label, so a search for a message must stay here.
	local conv = node({ role = "list", name = "Conversation" })
	if not check(conv, "the conversation has a name") then
		return
	end
	local function labels(text)
		return conv:find_all({ role = "label", name = text })
	end

	-- Send a message and wait for the canned reply to finish drawing.
	local replies = 0
	local function ask(text)
		entry:set_text(text)
		send:act()
		replies = replies + 1
		return conn:wait_for(function()
			return #labels("Yellow") >= replies
		end, 20)
	end

	check(ask("hello gtk"), "a reply arrives")
	-- "Fruit" is the heading and also the table's first column.
	check(#labels("Fruit") == 2, "the reply's heading and table header render")
	check(#labels("Orange") == 1, "the reply's bullets render")
	check(#labels("Banana") == 2, "the reply's table renders (cell and bullet)")
	local bullets = conv:find_all({ role = "label", name = "\u{2022}" })
	check(#bullets == 3, "list items get a bullet mark")
	check(conv:find({ role = "label", match = "^A bold word and an italic word" }),
	    "emphasis keeps its text")
	check(entry:text() == "", "the entry empties after a send")
	local reqs = requests()
	check(#reqs == 1 and reqs[1].model == "mock-model",
	    "the request names the configured model")
	check(reqs[1] and reqs[1].reasoning_effort == nil,
	    "default effort sends no reasoning_effort")

	-- The header's selectors.
	check(model:pick("mock/other-model", press), "pick a model")
	check(app:wait_find({ role = "label", name = "switched to mock/other-model" }, 5),
	    "the switch is noted in the chat")
	check(ask("second"), "a reply on the new model")
	reqs = requests()
	check(reqs[2] and reqs[2].model == "other-model",
	    "the next request uses the picked model")
	check(effort:pick("low", press), "pick an effort")
	check(ask("third"), "a reply at low effort")
	reqs = requests()
	check(reqs[3] and reqs[3].reasoning_effort == "low",
	    "the next request carries the picked effort")
	check(effort:pick("none", press), "pick effort none")
	check(ask("fourth"), "a reply at effort none")
	reqs = requests()
	check(reqs[4] and reqs[4].reasoning_effort == "none", "none is sent as none")
	check(effort:pick("default", press), "pick effort default")
	check(ask("fifth"), "a reply at default effort")
	reqs = requests()
	check(reqs[5] and reqs[5].reasoning_effort == nil,
	    "default goes back to the config's effort")

	-- Chat list: hidden at first, opens on the toggle, holds this chat.
	check(not chats:has_state("pressed"), "the chat list starts hidden")
	check(not app:find({ role = "button", name = "New chat" }),
	    "and its controls are not on screen")
	check(chats:act() and chats:wait_state("pressed", true, 5), "open the chat list")
	local list = node({ role = "list", name = "Saved chats" })
	local newchat = node({ role = "button", name = "New chat" })
	check(list and newchat, "the chat list and its new-chat button have names")
	if not (list and newchat) then
		return
	end
	local rows = list and conn:wait_for(function()
		local c = list:children() or {}
		return #c > 0 and c
	end, 5)
	check(rows and #rows >= 1, "the current chat is in the list")
	check(rows and rows[1]:name() == "hello gtk", "a row is named after its first message")

	-- A second chat, then back to the first through the list.
	check(newchat:act(), "new chat")
	check(conn:wait_for(function()
		return #labels("Yellow") == 0
	end, 5), "a new chat starts empty")
	replies = 0
	check(ask("a different chat"), "a reply in the new chat")
	rows = conn:wait_for(function()
		local c = list:children() or {}
		return #c >= 2 and c
	end, 5)
	check(rows and #rows == 2, "both chats are listed")
	local first
	for i, r in ipairs(rows or {}) do
		if r:name() == "hello gtk" then
			first = i - 1
		end
	end
	check(first and list:select_child(first), "select the first chat by accessibility")
	check(conn:wait_for(function()
		return conv:find({ role = "label", name = "hello gtk" })
	end, 5), "the first chat's messages come back")
	check(not conv:find({ role = "label", name = "a different chat" }),
	    "the other chat's messages are gone")

	-- Parity with the tui: peers, plugins, the shell escape, commands.
	local function socks()
		local n, names = 0, {}
		for _, f in ipairs(sys.listdir(tmp .. "/runtime/clm")) do
			if f:sub(-5) == ".sock" then
				n = n + 1
				names[#names + 1] = f:sub(1, -6)
			end
		end
		return n, names
	end
	local n_socks, names = socks()
	check(n_socks == 1, "the window announces itself on a peer socket")
	local function tool_names(req)
		local seen = {}
		for _, t in ipairs(req.tools or {}) do
			seen[(t["function"] or t).name] = true
		end
		return seen
	end
	local seen = tool_names(requests()[#requests()])
	check(seen.agents_list and seen.agent_send, "agents_list and agent_send are offered")
	check(seen.shell_exec, "the shell tool is offered")
	check(seen.reverse_string or seen.weather or seen.web_search,
	    "the seeded plugins load")

	-- "! cmd" runs here and reaches the model.
	local before = #requests()
	entry:set_text("! echo shellmark")
	send:act()
	check(conn:wait_for(function()
		return conv:find({ role = "label", match = "shellmark" })
	end, 10), "a ! command shows its output")
	check(conn:wait_for(function()
		return #requests() > before
	end, 20), "and the model is sent the output")
	local last = requests()[#requests()]
	local lastmsg = last.messages[#last.messages]
	check(type(lastmsg.content) == "string" and
	    lastmsg.content:find("[shell] $ echo shellmark", 1, true),
	    "in the [shell] form")
	replies = replies + 1

	-- A slash command that is not the model's.
	entry:set_text("/cd /tmp")
	send:act()
	check(conn:wait_for(function()
		return conv:find({ role = "label", match = "^cwd: /tmp" })
	end, 5), "/cd is handled by the window")

	-- A new chat moves the socket to the new session id.
	local old = names[1]
	check(newchat:act(), "new chat again")
	check(conn:wait_for(function()
		local n, nn = socks()
		return n == 1 and nn[1] ~= old
	end, 5), "a new chat moves the peer socket to the new session")

	-- /agent builds a new agent and keeps the peer tools.
	before = #requests()
	entry:set_text("/agent other")
	send:act()
	check(conn:wait_for(function()
		return conv:find({ role = "label", name = "switched to agent: other" })
	end, 10), "/agent switches the profile")
	check(conn:wait_for(function()
		local n = socks()
		return n == 1
	end, 5), "one socket after the switch")
	replies = 0
	check(ask("after the switch"), "a reply from the new agent")
	last = requests()[#requests()]
	seen = tool_names(last)
	check(last.model == "other-model", "the profile's model is used")
	check(seen.agents_list and seen.agent_send,
	    "the new agent still has the peer tools")

	-- An image a tool returns is shown as a thumbnail that opens on click.
	local png = tmp .. "/pic.png"
	os.execute("printf %s iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg== | base64 -d > " .. png)
	local opened = tmp .. "/opened"
	os.execute("mkdir -p " .. tmp .. "/bin && printf '#!/bin/sh\\necho \"$1\" > " .. opened
		.. "\\n' > " .. tmp .. "/bin/xdg-open && chmod +x " .. tmp .. "/bin/xdg-open")
	replies = replies + 0
	entry:set_text("imagetest " .. png)
	send:act()
	local thumb = conn:wait_for(function()
		return conv:find({ match = "^Open image" })
	end, 20)
	check(thumb, "a tool's image shows as a thumbnail button")
	check(thumb and thumb:name():find("png 1x1", 1, true), "named with its type and size")
	if thumb then
		check(thumb:act(), "click the thumbnail")
		check(conn:wait_for(function()
			local f = io.open(opened)
			if not f then return nil end
			local p = f:read("*l")
			f:close()
			return p and p:find("/clm/images/.*%.png$")
		end, 10), "it is handed to xdg-open as a png file")
	end

	-- Show-thinking toggle flips, with nothing to show.
	check(eye:act() and eye:wait_state("pressed", true, 5), "the thinking toggle presses")
	check(eye:act() and eye:wait_state("pressed", false, 5), "and releases")

	check(conn:wait_for(function()
		return app:find({ role = "label", name = "idle" })
	end, 5), "the window ends idle")
	if #failures > 0 then
		print((read_file(tmp .. "/gtk.log") or ""):sub(1, 2000))
	end
end

local ok, perr = pcall(run)
if not ok then
	check(false, "gtk test raised: " .. tostring(perr))
end
cleanup()

if #failures > 0 then
	print("\n" .. #failures .. " check(s) failed")
	return 1
end
print("\nall gtk checks passed")
return 0
