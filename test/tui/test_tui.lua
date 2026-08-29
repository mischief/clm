-- SPDX-License-Identifier: ISC
--
-- Deterministic TUI regression tests. They drive the real clm binary on a pty
-- against the built-in mock server and assert on the rendered terminal grid.
-- The binary under test comes from the CLM_BIN environment variable.

local here = arg[0]:match("^(.*)/[^/]*$") or "."
package.path = here .. "/?.lua;" .. package.path

local sys = require("sys")
local mock = require("mock")
local driver = require("driver")

driver.TEST_DIR = here:match("^(.*)/[^/]*$") or ".."

local BIN = os.getenv("CLM_BIN") or "clm"
local STATE_HOME = driver.STATE_HOME
local SESSIONS = STATE_HOME .. "/clm"

-- Blank columns the tui keeps between the transcript and each screen edge.
local MARGIN = 2

local failures = {}

local function check(cond, msg)
	print((cond and "ok  " or "FAIL") .. "  " .. msg)
	if not cond then
		failures[#failures + 1] = msg
	end
end

-- A condition the rest of the test cannot run without.
local function must(cond, msg)
	if not cond then
		error(msg, 0)
	end
end

local function has(s, sub)
	return s:find(sub, 1, true) ~= nil
end

local function at(s, sub)
	return s:find(sub, 1, true)
end

local function count(s, sub)
	local n = 0
	local i = 1

	while true do
		local a = s:find(sub, i, true)
		if a == nil then
			return n
		end
		n = n + 1
		i = a + #sub
	end
end

local function words(s)
	local out = {}

	for w in s:gmatch("%S+") do
		out[#out + 1] = w
	end
	return out
end

local function trim(s)
	return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- Transcript lines with the left margin removed.
local function body(t)
	local out = {}

	for i, ln in ipairs(t:lines()) do
		out[i] = ln:sub(1, MARGIN) == string.rep(" ", MARGIN) and
		    ln:sub(MARGIN + 1) or ln
	end
	return out
end

-- Every line but the status bar and the input box.
local function transcript_lines(t)
	local l = t:lines()
	local out = {}

	for i = 1, #l - 2 do
		out[i] = l[i]
	end
	return out
end

local function transcript(t)
	return table.concat(transcript_lines(t), "\n")
end

-- The status bar: the lowest line naming the binary and its state.
local function status_line(t)
	local l = t:lines()

	for i = #l, 1, -1 do
		if has(l[i], " clm ") and has(l[i], "[") then
			return l[i]
		end
	end
	return ""
end

local function last_line(t)
	local l = t:lines()

	return l[#l]
end

local function read_file(path)
	local f = io.open(path, "r")

	if f == nil then
		return nil
	end
	local s = f:read("a")
	f:close()
	return s
end

local function exists(path)
	return sys.stat(path) ~= nil
end

local function jsonl_set(dir)
	local out = {}

	for _, f in ipairs(sys.listdir(dir)) do
		if f:sub(-6) == ".jsonl" then
			out[f] = true
		end
	end
	return out
end

-- The .jsonl session logs that appeared since `before` was taken.
local function new_sessions(before)
	local out = {}

	for f in pairs(jsonl_set(SESSIONS)) do
		if not before[f] then
			out[#out + 1] = f
		end
	end
	table.sort(out)
	return out
end

local function nonblank(lines)
	local out = {}

	for _, ln in ipairs(lines) do
		if trim(ln) ~= "" then
			out[#out + 1] = ln
		end
	end
	return out
end

local tests = {}

tests.connection = function(url)
	local t = driver.new(BIN, url, { rows = 12, cols = 60 })

	check(t:wait_for("[online]", 10),
	    "connection: status bar shows [online] against a live server")
	t:close()
end

tests.offline = function()
	-- Point at a port nothing is listening on; expect an offline marker.
	local t = driver.new(BIN, "http://127.0.0.1:1/v1/chat/completions",
	    { rows = 12, cols = 60 })

	check(t:wait_for("offline", 10),
	    "connection: status bar shows offline when unreachable")
	t:close()
end

tests.agent = function(url)
	local t = driver.new(BIN, url, { rows = 12, cols = 60 })

	if not t:wait_for("[online]", 10) then
		t:pump(1.0)
	end
	-- test/config/clm/config.lua resolves agent "test" to the model
	-- "mock/mock-model".
	check(has(t:text(), "[mock/mock-model:test]"),
	    "agent: status bar shows provider/model:agent from config")
	t:close()
end

tests.markdown = function(url)
	local t = driver.new(BIN, url, { rows = 24, cols = 70 })

	t:send("show me fruit\r")
	-- "Yellow" is the last table cell, so waiting for it means the whole
	-- streamed reply has arrived.
	must(t:wait_for("Yellow", 15), "no response rendered")
	local txt = t:text()
	check(has(txt, "Fruit"), "markdown: heading text present")
	check(has(txt, "Apple") and has(txt, "Banana"),
	    "markdown: list/table items")
	check(has(txt, "│") or has(txt, "─"),
	    "markdown: table drawn with box characters")
	check(t:any_bold("bold"), "markdown: **bold** rendered bold")
	t:close()
end

tests.scrollback = function(url)
	-- A short terminal, so the canned reply overflows and can be paged.
	local t = driver.new(BIN, url, { rows = 10, cols = 40 })

	t:send("show me fruit\r")
	must(t:wait_for("Colour", 15) or t:wait_for("Fruit", 5),
	    "no response to scroll")
	local bottom = t:text()
	t:send(driver.PGUP)
	t:send(driver.PGUP)
	t:pump(0.4)
	local scrolled = t:text()
	check(scrolled ~= bottom, "scrollback: PgUp changes the viewport")
	check(has(scrolled, "Fruit"),
	    "scrollback: PgUp reveals earlier content")
	t:send(driver.PGDN)
	t:send(driver.PGDN)
	t:send(driver.PGDN)
	t:pump(0.4)
	check(t:text() ~= scrolled, "scrollback: PgDn returns toward bottom")
	t:close()
end

tests.scroll_stable = function(url)
	-- Scroll is stored as a distance from the bottom, so a growing
	-- transcript must not drag the viewport forward under a reader.
	-- Only the transcript rows are compared: the status bar's spinner
	-- animates independent of scroll.
	local t = driver.new(BIN, url, { rows = 10, cols = 40 })

	t:send("show me fruit\r")
	must(t:wait_for("Colour", 15), "no first response")
	t:send(driver.PGUP)
	t:send(driver.PGUP)
	t:pump(0.4)
	local scrolled = transcript(t)
	check(has(scrolled, "Fruit"), "scroll-stable: scrolled up into history")

	-- Submit a second prompt while scrolled up and sample the viewport
	-- partway through its reply.
	t:send("slowtest show me fruit again\r")
	t:pump(0.3)
	check(transcript(t) == scrolled,
	    "scroll-stable: viewport unchanged partway through a new " ..
	    "streamed reply while scrolled up")

	t:pump(1.5)
	check(transcript(t) == scrolled,
	    "scroll-stable: viewport still unchanged once the new reply " ..
	    "finishes streaming")

	-- Following the bottom must still work once the user scrolls back.
	t:send(driver.PGDN)
	t:send(driver.PGDN)
	t:send(driver.PGDN)
	t:pump(0.4)
	check(transcript(t) ~= scrolled,
	    "scroll-stable: PgDn still returns toward the bottom")
	t:close()
end

tests.end_key = function(url)
	-- End on an empty input line jumps the transcript to the bottom; End
	-- while editing a line still only moves the cursor.
	local t = driver.new(BIN, url, { rows = 10, cols = 40 })

	t:send("show me fruit\r")
	must(t:wait_for("Colour", 15), "no first response")
	t:send(driver.PGUP)
	t:send(driver.PGUP)
	t:pump(0.4)
	local before = transcript(t)
	check(has(before, "Fruit"), "end: scrolled up into history")

	t:send(driver.END) -- input is empty here
	t:pump(0.4)
	local after = transcript(t)
	check(after ~= before,
	    "end: jumps the transcript to the bottom from an empty input")
	check(has(after, "Colour"),
	    "end: bottom of the transcript is now visible")

	t:send(driver.PGUP)
	t:send(driver.PGUP)
	t:pump(0.4)
	t:send("hello")
	t:pump(0.2)
	local typed = transcript(t)
	t:send(driver.END)
	t:pump(0.3)
	check(transcript(t) == typed,
	    "end: with text typed, only moves the input cursor, " ..
	    "doesn't scroll")
	t:close()
end

tests.resize = function(url)
	local t = driver.new(BIN, url, { rows = 24, cols = 70 })

	t:send("show me fruit\r")
	must(t:wait_for("Apple", 15), "no response before resize")
	t:resize(20, 40)
	check(has(t:text(), "Apple"), "resize: content survives narrow reflow")
	t:resize(28, 100)
	check(has(t:text(), "Apple"), "resize: content survives wide reflow")
	t:close()
end

tests.margin = function(url)
	local cols = 60
	local t = driver.new(BIN, url, { rows = 24, cols = cols })

	t:send("show me fruit\r")
	must(t:wait_for("Apple", 15), "no response to measure")
	local rows = nonblank(transcript_lines(t))
	local all_indented = #rows > 0
	local widest = 0
	for _, ln in ipairs(rows) do
		if ln:sub(1, MARGIN) ~= string.rep(" ", MARGIN) then
			all_indented = false
		end
		local w = utf8.len(ln) or #ln
		if w > widest then
			widest = w
		end
	end
	check(all_indented,
	    "margin: no transcript line starts at the left edge")
	check(widest > cols - 2 * MARGIN - 8,
	    "margin: some line is long enough to reach the right edge")
	check(widest <= cols - MARGIN,
	    "margin: no transcript line runs into the right edge")
	t:close()

	-- The gutters belong to no window. Whatever a full-width window drew
	-- there must not survive it moving or shrinking.
	t = driver.new(BIN, url, { rows = 14, cols = cols })
	t:wait_for("online", 8)
	t:send("show me fruit\r")
	must(t:wait_for("Apple", 15), "no response before resize")
	t:send(string.rep("x", 200)) -- grow the input box
	t:pump(0.6)
	t:send(driver.CTRL_U) -- shrink it back, vacating those rows
	t:pump(0.6)
	local left_clear = true
	local right_clear = true
	local lines = t:lines()
	for i = 1, #lines - 2 do
		if trim(lines[i]:sub(1, MARGIN)) ~= "" then
			left_clear = false
		end
		if (utf8.len(lines[i]) or #lines[i]) > cols - MARGIN then
			right_clear = false
		end
	end
	check(left_clear, "margin: the left gutter is clear after the box shrinks")
	check(right_clear,
	    "margin: the right gutter is clear after the box shrinks")
	t:close()

	-- A screen too narrow to spare the columns drops the margin rather
	-- than squeezing the text.
	t = driver.new(BIN, url, { rows = 10, cols = 7 })
	t:send("show me fruit\r")
	t:pump(3.0)
	rows = nonblank(transcript_lines(t))
	local any_flush = false
	for _, ln in ipairs(rows) do
		if trim(ln:sub(1, 1)) ~= "" then
			any_flush = true
		end
	end
	check(#rows > 0 and any_flush,
	    "margin: dropped when the screen cannot spare it")
	t:close()
end

tests.editing = function(url)
	local t = driver.new(BIN, url, { rows = 12, cols = 60 })

	t:send("hello world")
	t:pump(0.3)
	check(has(t:text(), "hello world"), "edit: typed text shows in input")
	t:send(driver.CTRL_A) -- cursor to start
	t:send(driver.CTRL_K) -- kill to end (the whole line)
	t:pump(0.3)
	local bare = trim(last_line(t))
	check(bare == ">" or bare == "",
	    "edit: ^A then ^K clears the input line")
	t:send(driver.CTRL_Y) -- yank it back
	t:pump(0.3)
	check(has(t:text(), "hello world"),
	    "edit: ^Y yanks the killed text back")
	t:send(driver.CTRL_U) -- kill to start, clearing it again
	t:pump(0.3)
	check(not has(last_line(t), "hello world"),
	    "edit: ^U clears the restored line")
	t:close()
end

tests.history = function(url)
	local t = driver.new(BIN, url, { rows = 12, cols = 60 })

	t:wait_for("online", 8)
	t:send("alpha one\r")
	t:pump(0.3)
	t:send("bravo two\r")
	t:pump(0.3)
	t:send(driver.UP) -- most recent
	t:pump(0.3)
	check(has(last_line(t), "bravo two"),
	    "history: Up recalls the most recent prompt")
	t:send(driver.UP) -- older
	t:pump(0.3)
	check(has(last_line(t), "alpha one"),
	    "history: Up again recalls the older prompt")
	t:send(driver.DOWN)
	t:pump(0.3)
	check(has(last_line(t), "bravo two"),
	    "history: Down returns toward the newer prompt")
	t:close()
end

tests.commands = function(url)
	local t = driver.new(BIN, url, { rows = 20, cols = 80 })

	t:wait_for("online", 8)
	t:send("/help\r")
	t:pump(0.4)
	check(has(t:text(), "/clear") and has(t:text(), "/reasoning"),
	    "commands: /help lists commands")
	t:send("show me fruit\r")
	must(t:wait_for("Yellow", 15), "no reply before clear")
	t:pump(1.0) -- let the stream finish so nothing re-appends after clear
	t:send("/clear\r")
	t:pump(0.5)
	check(not has(t:text(), "Apple"),
	    "commands: /clear wipes the transcript")
	t:close()
end

tests.effort = function(url)
	local t = driver.new(BIN, url, { rows = 20, cols = 70 })

	t:wait_for("online", 8)
	t:send("/effort\r")
	t:pump(0.4)
	check(has(t:text(), "effort:"),
	    "effort: bare command reports the level")
	t:send("/effort low\r")
	t:pump(0.4)
	check(has(t:text(), "effort: low"), "effort: level accepted")
	t:send("/effort \t")
	t:pump(0.5)
	local txt = t:text()
	check(has(txt, "xhigh") and has(txt, "medium"),
	    "effort: TAB lists the levels")
	t:send(driver.CTRL_U)
	t:pump(0.2)
	t:send("/effo\t")
	t:pump(0.5)
	check(has(t:text(), "/effort"), "effort: command name completes")
	t:send(driver.CTRL_U)
	t:close()
end

tests.queueing = function(url)
	local t = driver.new(BIN, url, { rows = 20, cols = 70 })

	t:wait_for("online", 8)
	-- The first prompt starts a slow turn; the second queues mid-turn.
	t:send("slowtest first\r")
	t:pump(0.15)
	t:send("second\r")
	check(t:wait_for("(queued)", 3),
	    "queueing: a prompt sent while busy shows the queue marker")
	t:pump(6.0)
	check(not has(t:text(), "turn already in progress"),
	    "queueing: no in-progress rejection reaches the transcript")
	t:send("/quit\r")
	t:pump(0.8)
	t:close()

	-- The queued line reaching the agent is the point, and the session
	-- log is a steadier witness than a grid mid-repaint.
	local logged = ""
	for _, f in ipairs(sys.listdir(SESSIONS)) do
		if f:sub(-6) == ".jsonl" then
			logged = logged .. (read_file(SESSIONS .. "/" .. f) or "")
		end
	end
	check(has(logged, '"second"'),
	    "queueing: the queued prompt is submitted when the turn ends")

	-- Same path from idle: a prompt typed with no turn running still has
	-- to reach the agent.
	t = driver.new(BIN, url, { rows = 20, cols = 70 })
	t:wait_for("online", 8)
	t:send("show me fruit\r")
	check(t:wait_for("Yellow", 15),
	    "queueing: an idle prompt still runs a turn")
	check(not has(t:text(), "turn already in progress"),
	    "queueing: idle submit is not rejected")
	t:close()
end

tests.permission = function(url)
	-- "shelltest" makes the mock emit a shell_exec call, which the tui
	-- gates behind the permission prompt.
	local t = driver.new(BIN, url, { rows = 24, cols = 80 })

	t:wait_for("online", 8)
	t:send("shelltest please\r")
	must(t:wait_for("allow tool", 15), "no permission prompt")
	local txt = t:text()
	check(has(txt, "shell_exec"), "permission: prompt names the tool")
	check(has(txt, "command: echo hi"),
	    "permission: single-line parameter stays beside its name")
	check(has(txt, "(y) once"), "permission: prompt shows the key choices")
	t:send("y")
	t:pump(1.0)
	check(has(t:text(), "allowed"), "permission: 'y' approves the call")
	t:close()

	-- A multi-line argument gets its own indented block. The screen is
	-- tall enough that the follow-up reply does not scroll the summary
	-- away before it is read.
	t = driver.new(BIN, url, { rows = 40, cols = 80 })
	t:wait_for("online", 8)
	t:send("multilinetest please\r")
	must(t:wait_for("allow tool", 15), "no multiline permission prompt")
	txt = t:text()
	check(has(txt, "command:") and has(txt, "printf one") and
	    has(txt, "doas true"),
	    "permission: multiline value is shown below its name")
	check(has(txt, "stdin:") and has(txt, "first line") and
	    has(txt, "second line"),
	    "permission: multiline argument contents are retained")
	check(has(txt, "timeout_ms: 10000"),
	    "permission: later single-line parameter remains readable")
	-- Approve, then check the one-line tool summary: a newline inside the
	-- command is escaped, not flattened into a space.
	t:send("y")
	t:pump(1.0)
	local lines = {}
	for _, ln in ipairs(body(t)) do
		lines[ln] = true
	end
	check(lines["  executing shell command"],
	    "tool summary: verb on its own line")
	check(lines["    printf one"] and lines["    cat /tmp/x"] and
	    lines["    doas true"],
	    "tool summary: each command line drawn on its own line")
	t:close()

	-- An edit's arguments follow the order the tool declares, not the
	-- order the model emitted them.
	t = driver.new(BIN, url, { rows = 24, cols = 80 })
	t:wait_for("online", 8)
	t:send("edittest please\r")
	must(t:wait_for("allow tool", 15), "no edit permission prompt")
	txt = t:text()
	check(has(txt, "old_str: before text") and
	    has(txt, "new_str: after text"),
	    "permission: edit arguments are shown")
	check(at(txt, "path: /tmp/x") < at(txt, "old_str: before text") and
	    at(txt, "old_str: before text") < at(txt, "new_str: after text"),
	    "permission: path, then replaced text, then replacement")
	check(at(txt, "new_str: after text") < at(txt, "replace_all: false"),
	    "permission: unordered arguments follow the named ones")
	t:send("n")
	t:pump(1.0)
	t:close()

	-- A fresh session, deny this time.
	t = driver.new(BIN, url, { rows = 24, cols = 80 })
	t:wait_for("online", 8)
	t:send("shelltest please\r")
	must(t:wait_for("allow tool", 15), "no permission prompt (deny)")
	t:send("n")
	t:pump(1.0)
	check(has(t:text(), "denied"), "permission: 'n' denies the call")
	t:close()
end

tests.cancel = function(url)
	local t = driver.new(BIN, url, { rows = 20, cols = 70 })

	t:wait_for("online", 8)
	t:send("slowtest show me fruit\r")
	t:pump(0.15) -- the turn is now streaming
	t:send(driver.ESCAPE)
	must(t:wait_for("[cancelled]", 5), "cancel not reflected")
	check(has(t:text(), "[cancelled]"),
	    "cancel: Escape cancels an in-flight turn")
	t:send("hello again\r")
	t:pump(0.3)
	check(has(t:text(), "hello again"),
	    "cancel: input works after cancel")
	t:close()
end

tests.cancel_tools = function(url)
	-- Escape must stop the calls a batch has not started yet, not just
	-- the ones already running.
	local scratch = mock.scratch()
	local many = mock.many_calls()

	for _, f in ipairs(sys.listdir(scratch)) do
		os.remove(scratch .. "/" .. f)
	end
	local t = driver.new(BIN, url,
	    { rows = 20, cols = 80, args = { "--allow-all-tools" } })
	t:wait_for("online", 8)
	t:send("manytest please\r")
	must(t:wait_for("executing shell command", 15), "no tool call")
	t:pump(0.5)
	local started = #sys.listdir(scratch)
	check(started < many,
	    "cancel: the rate limit leaves part of the batch parked")
	t:send(driver.ESCAPE)
	must(t:wait_for("cancelled", 8), "cancel not reflected")
	for _ = 1, 6 do
		t:pump(1.0)
	end
	check(#sys.listdir(scratch) <= started + 1,
	    "cancel: parked calls do not run after the turn is cancelled")
	t:send("hello again\r")
	check(t:wait_for("Apple", 15),
	    "cancel: a new turn runs after cancel")
	t:close()
end

tests.tool_escapes = function(url)
	-- Overstrike and escape sequences never reach the transcript.
	local t = driver.new(BIN, url,
	    { rows = 40, cols = 80, args = { "--allow-all-tools" } })

	t:wait_for("online", 8)
	t:send("escapetest please\r")
	must(t:wait_for("NAME", 15), "tool output never arrived")
	-- The echoed command line holds the escapes as literal text, so look
	-- at the tool's own output line, not the whole screen.
	local out = nil
	for _, ln in ipairs(t:lines()) do
		if has(ln, "NAME") and out == nil then
			out = trim(ln)
		end
	end
	check(out == "NAME int red",
	    "escapes: overstrike collapses and the colour escape is gone")
	t:close()
end

tests.paste = function(url)
	-- A pasted multi-line block must land as one submitted turn, not one
	-- per embedded newline.
	local t = driver.new(BIN, url, { rows = 24, cols = 80 })

	t:wait_for("online", 8)
	t:send(driver.PASTE_START .. "line one\rline two\rline three" ..
	    driver.PASTE_END)
	t:pump(0.3)
	check(not has(t:text(), "you>"),
	    "paste: embedded newlines don't auto-submit while pasting")
	check(has(t:text(), "line one") and has(t:text(), "line three"),
	    "paste: all three lines landed in the input box")

	t:send("\r")
	must(t:wait_for("Apple", 15), "no reply after submitting paste")
	local txt = t:text()
	check(count(txt, "you>") == 1,
	    "paste: the whole paste became exactly one submitted turn")
	check(has(txt, "line one") and has(txt, "line two") and
	    has(txt, "line three"),
	    "paste: all three lines appear in the submitted turn")
	t:close()

	-- The caret belongs after the last pasted character. The box lays the
	-- text out with newlines as hard breaks.
	t = driver.new(BIN, url, { rows = 24, cols = 80 })
	t:wait_for("online", 8)
	t:send("shape: ")
	t:send(driver.PASTE_START ..
	    "    typedef int (*handler)(int fd, void *user);\r\r" ..
	    "    int add_io(int fd, handler h, void *user);" ..
	    driver.PASTE_END)
	t:pump(0.5)
	local lines = t:lines()
	local last = 1
	for i, ln in ipairs(lines) do
		if trim(ln) ~= "" then
			last = i
		end
	end
	local cy, cx = t:cursor()
	check(cy == last - 1 and cx == (utf8.len(lines[last]) or #lines[last]),
	    "paste: the caret sits after the last pasted character")
	t:close()

	-- A paste far past the old fixed 1 KiB line buffer must arrive whole.
	t = driver.new(BIN, url, { rows = 24, cols = 80 })
	t:wait_for("online", 8)
	local parts = {}
	for i = 0, 119 do
		parts[#parts + 1] = string.format(
		    "log line %03d filler filler filler", i)
	end
	local big = table.concat(parts, "\r")
	check(#big > 4000, "paste: fixture is well past the old limit")
	t:send(driver.PASTE_START .. big .. driver.PASTE_END)
	t:pump(0.6)
	t:send("\r")
	must(t:wait_for("Apple", 15), "no reply after big paste")
	t:pump(0.5)
	t:send("/quit\r")
	t:pump(0.8)
	t:close()

	local logged = ""
	for _, f in ipairs(sys.listdir(SESSIONS)) do
		if f:sub(-6) == ".jsonl" then
			logged = logged .. (read_file(SESSIONS .. "/" .. f) or "")
		end
	end
	check(has(logged, "log line 000") and has(logged, "log line 119"),
	    "paste: a paste past the old 1 KiB cap arrives whole")
end

tests.session = function(url)
	-- A conversation is logged to a session file, and --resume replays it.
	local before = jsonl_set(SESSIONS)
	local t = driver.new(BIN, url, { rows = 24, cols = 110 })

	t:wait_for("online", 8)
	t:send("show me fruit\r")
	must(t:wait_for("Yellow", 15), "no reply to log")
	t:pump(0.5)
	-- /session names the log this run writes, and the status bar carries
	-- the same short id.
	t:send("/session\r")
	t:pump(0.5)
	local txt = t:text()
	check(has(txt, "session: ") and has(txt, "resume: clm --resume"),
	    "session: /session prints the id and how to resume it")
	local short = nil
	for ln in txt:gmatch("[^\n]+") do
		if trim(ln):sub(1, 6) == "short:" then
			local w = words(ln)
			short = w[#w]
		end
	end
	check(short ~= nil and #short == 8,
	    "session: /session reports an 8-character short id")
	check(short ~= nil and has(status_line(t), short),
	    "session: the status bar carries the short id")
	t:send("/quit\r")
	t:pump(0.8)
	t:close()

	local created = new_sessions(before)
	check(#created >= 1, "session: a .jsonl session log was written")
	must(#created >= 1, "no session log to resume")
	local file = created[#created]
	local sid = file:sub(1, #file - 6)
	check(short == nil or sid:sub(-8) == short,
	    "session: the short id is the tail of the full one")
	local log = read_file(SESSIONS .. "/" .. file) or ""
	check(has(log, '"type": "meta"') or has(log, '"type":"meta"'),
	    "session: log starts with a meta line")
	check(has(log, "show me fruit"), "session: the user prompt was logged")

	t = driver.new(BIN, url, { rows = 24, cols = 70,
	    args = { "--resume", sid:sub(-8) } })
	must(t:wait_for("resumed session", 10), "no resume banner")
	txt = t:text()
	check(has(txt, "show me fruit"),
	    "resume: the old prompt is replayed in the transcript")
	check(has(txt, "Apple") or has(txt, "Fruit"),
	    "resume: the old reply is replayed in the transcript")
	-- The resumed conversation must accept a new turn.
	t:send("show me fruit\r")
	check(t:wait_for("Yellow", 15),
	    "resume: a follow-up turn works after resuming")
	t:close()
end

tests.resume_collapse = function(url)
	-- A replayed transcript folds older tool clusters, same as a live one.
	local before = jsonl_set(SESSIONS)
	local t = driver.new(BIN, url,
	    { rows = 40, cols = 100, args = { "--allow-all-tools" } })

	t:wait_for("online", 8)
	t:send("shelltest one\r")
	must(t:wait_for("Yellow", 15), "no reply to the first turn")
	t:send("shelltest two\r")
	must(t:wait_for("ran 1 command", 15), "no second cluster")
	t:pump(1.0)
	t:send("/quit\r")
	t:pump(0.8)
	t:close()

	local created = new_sessions(before)
	if #created == 0 then
		check(false, "resume: a session log was written")
		return
	end
	local file = created[#created]
	local sid = file:sub(1, #file - 6)

	t = driver.new(BIN, url, { rows = 40, cols = 100,
	    args = { "--resume", sid:sub(-8) } })
	must(t:wait_for("resumed session", 10), "no resume banner")
	t:pump(0.5)
	local txt = t:text()
	check(has(txt, "(^O to expand)"),
	    "resume: an older tool cluster is collapsed")
	local head = txt:sub(1, (at(txt, "shelltest two") or #txt) - 1)
	check(not has(head, "shell_exec"),
	    "resume: the collapsed cluster's own lines are gone")
	t:close()
end

tests.session_compact = function(url)
	-- /compact rewrites the session log, so a resume starts compacted.
	local before = jsonl_set(SESSIONS)
	local t = driver.new(BIN, url, { rows = 24, cols = 70 })

	t:wait_for("online", 8)
	for _ = 1, 3 do
		t:send("show me fruit\r")
		must(t:wait_for("Yellow", 15), "no reply to log")
		t:pump(0.3)
	end
	t:send("/compact\r")
	must(t:wait_for("compacting", 10), "no compaction started")
	t:pump(3.0)
	t:send("/quit\r")
	t:pump(0.8)
	t:close()

	local created = new_sessions(before)
	check(#created == 1, "compact: one session log for the run")
	must(#created >= 1, "no session log to inspect")
	local path = SESSIONS .. "/" .. created[1]
	local kinds = {}
	local msgs = {}
	for ln in (read_file(path) or ""):gmatch("[^\n]+") do
		if trim(ln) ~= "" then
			local rec = json.decode(ln)
			kinds[#kinds + 1] = rec.type
			if #kinds > 1 then
				msgs[#msgs + 1] = rec
			end
		end
	end
	check(kinds[1] == "meta", "compact: rewritten log keeps its meta line")
	local all_msg = true
	local no_system = true
	local users = {}
	for i = 2, #kinds do
		if kinds[i] ~= "msg" then
			all_msg = false
		end
	end
	for _, m in ipairs(msgs) do
		if m.role == "system" then
			no_system = false
		end
		if m.role == "user" then
			users[#users + 1] = m.content or ""
		end
	end
	check(all_msg, "compact: every later line is a message")
	check(no_system,
	    "compact: the rebuilt-on-resume system prologue stays out")
	-- The mock answers the summarize call with its usual reply, so the
	-- summary shows up as a user message carrying assistant-looking text.
	local summarized = false
	for _, c in ipairs(users) do
		if has(c, "Fruit") then
			summarized = true
		end
	end
	check(summarized, "compact: the summary replaced the folded turns")
	check(#users < 4, "compact: fewer prompts remain than were typed")
	check(not exists(path .. ".tmp"),
	    "compact: no temporary file left behind")

	-- One cycle of the pre-compaction log survives beside it.
	local bak = path .. ".bak"
	check(exists(bak), "compact: the previous log is kept as .bak")
	if exists(bak) then
		local bak_users = 0
		for ln in (read_file(bak) or ""):gmatch("[^\n]+") do
			if trim(ln) ~= "" and json.decode(ln).role == "user" then
				bak_users = bak_users + 1
			end
		end
		check(bak_users >= 3,
		    "compact: the backup still holds the folded turns")
	end
end

tests.clear_gauge = function(url)
	-- /clear drops the context gauge along with the history it measured.
	local t = driver.new(BIN, url, { rows = 24, cols = 80 })

	t:wait_for("online", 8)
	t:send("show me fruit\r")
	must(t:wait_for("Yellow", 15), "no reply")
	t:pump(0.5)
	check(has(status_line(t), "%"),
	    "clear_gauge: the gauge appears once a turn reports usage")
	t:send("/clear\r")
	t:pump(0.8)
	check(not has(status_line(t), "%"),
	    "clear_gauge: /clear takes the gauge away with the history")
	t:close()
end

tests.scratch = function(url)
	-- Each session gets a private scratch dir, named in the prompt and
	-- the environment so the agent puts working files there.
	local cache = STATE_HOME .. "/cache"
	local reqlog = STATE_HOME .. "/scratch-requests.jsonl"

	mock.request_log(reqlog)
	local t = driver.new(BIN, url, { rows = 20, cols = 100 })
	t:wait_for("online", 8)
	t:send("/session\r")
	t:pump(0.6)
	local sid = nil
	for ln in t:text():gmatch("[^\n]+") do
		if trim(ln):sub(1, 8) == "session:" then
			local w = words(ln)
			sid = w[#w]
		end
	end
	check(sid ~= nil, "scratch: session id available")
	t:send("say something\r") -- one request, so the prompt goes out
	t:pump(1.5)
	t:send("/quit\r")
	t:pump(0.8)
	t:close()
	mock.request_log(nil)

	-- The system prompt names the session, so an agent can tell another
	-- one where to reach it. Read it off the wire: the session log leaves
	-- the prologue out on purpose.
	local prologue = ""
	for ln in (read_file(reqlog) or ""):gmatch("[^\n]+") do
		for _, m in ipairs(json.decode(ln).messages or {}) do
			if m.role == "system" then
				prologue = m.content or ""
			end
		end
	end
	check(sid ~= nil and has(prologue, "this session's id: " .. sid),
	    "scratch: the prompt names this session's id")

	local root = cache .. "/clm/scratch"
	local st = sys.stat(root)
	check(st ~= nil and st.dir, "scratch: the scratch root is created")
	if st ~= nil and sid ~= nil then
		local found = false
		for _, f in ipairs(sys.listdir(root)) do
			if f == sid then
				found = true
			end
		end
		check(found, "scratch: the directory is named after the session")
		local dst = sys.stat(root .. "/" .. sid)
		check(dst ~= nil and dst.mode == tonumber("700", 8),
		    "scratch: private to the user")
	end
end

tests.allow_all = function(url)
	-- --allow-all-tools runs tool calls without the permission prompt,
	-- and says so where it cannot be missed.
	local t = driver.new(BIN, url,
	    { rows = 20, cols = 100, args = { "--allow-all-tools" } })

	t:wait_for("online", 8)
	check(has(status_line(t), "[allow-all]"),
	    "allow-all: the status bar says the session is ungated")
	t:send("shelltest please\r")
	-- The gated path would stop here waiting for y/n.
	check(t:wait_for("hi", 15), "allow-all: the tool runs without asking")
	check(not has(t:text(), "allow tool"),
	    "allow-all: no permission prompt appeared")
	t:close()
end

tests.peers = function(url)
	-- Two clm instances discover each other over their sockets, and a
	-- message from one lands in the other's transcript between turns.
	local run = STATE_HOME .. "/run"

	sys.mkdir(run)
	sys.setenv("XDG_RUNTIME_DIR", run)
	local d = run .. "/clm"

	local a = driver.new(BIN, url, { rows = 30, cols = 100 })
	local b = driver.new(BIN, url, { rows = 30, cols = 100 })
	a:wait_for("online", 8)
	b:wait_for("online", 8)

	local socks = {}
	local metas = {}
	for _, f in ipairs(sys.listdir(d)) do
		if f:sub(-5) == ".sock" then
			socks[#socks + 1] = f
		elseif f:sub(-5) == ".json" then
			metas[#metas + 1] = f
		end
	end
	check(#socks == 2, "peers: each instance binds its own socket")
	check(#metas == 2, "peers: each announces itself for discovery")
	if #metas > 0 then
		local meta = json.decode(read_file(d .. "/" .. metas[1]) or "{}")
		check(meta.id and meta.name and meta.model and meta.cwd and
		    meta.pid, "peers: the announcement carries who and where")
	end

	local function short_id(t)
		local id = nil
		for _, ln in ipairs(t:lines()) do
			if has(ln, " clm ") then
				id = words(ln)[2]
			end
		end
		return id
	end

	local b_id = short_id(b)
	check(b_id ~= nil, "peers: target's short id is on screen")

	-- /clear starts a new session, and the socket carries the session id.
	b:send("/clear\r")
	b:pump(2.0)
	local new_id = short_id(b)
	check(new_id ~= nil and new_id ~= b_id,
	    "peers: /clear gives the instance a new session id")
	local moved = false
	local stale = false
	for _, f in ipairs(sys.listdir(d)) do
		if f:sub(-5) == ".sock" then
			local stem = f:sub(1, #f - 5)
			if new_id ~= nil and stem:sub(-#new_id) == new_id then
				moved = true
			end
			if b_id ~= nil and stem:sub(-#b_id) == b_id then
				stale = true
			end
		end
	end
	check(moved, "peers: /clear moves the socket to the new session id")
	check(not stale, "peers: the socket for the cleared session is gone")

	-- The mock turns "peersend to=<id>" into one agent_send call.
	a:send("peersend to=" .. new_id .. " please\r")
	must(a:wait_for("allow tool agent_send", 15),
	    "no permission prompt for agent_send")
	check(true, "peers: sending to another agent asks first")
	a:send("y")
	a:pump(2.5)
	b:pump(2.0)
	check(has(b:text(), "hello from the other agent"),
	    "peers: the message reaches the agent at its new id")
	check(has(b:text(), "peer "),
	    "peers: the transcript marks it as coming from a peer")

	-- The announcement is what a listing reports, so it has to track a
	-- model switch.
	local b_meta = nil
	for _, f in ipairs(sys.listdir(d)) do
		if f:sub(-5) == ".json" and
		    f:sub(1, #f - 5):sub(-#new_id) == new_id then
			b_meta = f
		end
	end
	check(b_meta ~= nil, "peers: the cleared session announces itself")
	if b_meta ~= nil then
		b:send("/model other-model\r")
		b:pump(1.5)
		local meta = json.decode(read_file(d .. "/" .. b_meta) or "{}")
		check(meta.model == "other-model",
		    "peers: /model updates the announced model")
	end
	a:close()
	b:close()

	-- Both instances are gone: their sockets must not be left behind.
	local leftover = 0
	for _, f in ipairs(sys.listdir(d)) do
		if f:sub(-5) == ".sock" then
			leftover = leftover + 1
		end
	end
	check(leftover == 0, "peers: sockets are removed on exit")

	-- A killed instance cleans up too.
	local function count_socks()
		local n = 0
		for _, f in ipairs(sys.listdir(d)) do
			if f:sub(-5) == ".sock" then
				n = n + 1
			end
		end
		return n
	end

	local t = driver.new(BIN, url, { rows = 30, cols = 100 })
	t:wait_for("online", 8)
	check(count_socks() > 0, "peers: the instance bound a socket")
	sys.kill(t.pid, sys.SIGTERM)
	local deadline = sys.now() + 5
	while sys.now() < deadline and count_socks() > 0 do
		sys.sleep(0.1)
	end
	check(count_socks() == 0, "peers: SIGTERM removes the socket")
	t:close()

	-- A headless run can message a running agent without advertising
	-- itself: it exits in seconds, so a socket answering for it would
	-- only mislead whoever found it.
	t = driver.new(BIN, url, { rows = 30, cols = 100 })
	t:wait_for("online", 8)
	local tui_id = short_id(t)
	local code, out = sys.run({ BIN, "-u", url, "-m", "mock-model", "-o",
	    "peersend to=" .. tostring(tui_id) .. " please" },
	    { CLM_YOLO = "1" })
	check(code == 0, "peers: the headless run completes")
	check(has(out, "delivered to"),
	    "peers: a headless run can send to a running agent")
	check(count_socks() == 1,
	    "peers: the headless run does not announce itself")
	t:pump(2.0)
	check(has(t:text(), "hello from the other agent"),
	    "peers: the running agent receives it")
	t:close()
end

-- The order tests run in when none are named.
local ORDER = { "connection", "offline", "agent", "markdown", "scrollback",
	"scroll_stable", "end_key", "resize", "margin", "editing", "history",
	"commands", "effort", "queueing", "permission", "cancel",
	"cancel_tools", "tool_escapes", "paste", "session", "resume_collapse",
	"session_compact", "clear_gauge", "scratch", "peers", "allow_all" }

local selected = {}
for i = 1, #arg do
	selected[#selected + 1] = arg[i]
end
if #selected == 0 then
	selected = ORDER
end
for _, name in ipairs(selected) do
	if tests[name] == nil then
		io.stderr:write("unknown TUI test: " .. name .. "\n")
		return 2
	end
end

local url = mock.start()
for _, name in ipairs(selected) do
	local ok, err = pcall(tests[name], url)
	if not ok then
		check(false, name .. ": " .. tostring(err))
	end
end
mock.stop()
sys.rmtree(STATE_HOME)

if #failures > 0 then
	print("\n" .. #failures .. " check(s) failed")
	return 1
end
print("\nall TUI checks passed")
return 0
