-- SPDX-License-Identifier: ISC
--
-- A pty driver for the clm TUI under test. It starts the real binary on a
-- pseudo-terminal of a chosen size, points it at the mock server, and renders
-- its output through vt.lua, so a test can assert on the grid a user would
-- see and drive keystrokes and resizes the way a terminal does.

local sys = require("sys")
local vt = require("vt")

local M = {}

-- Shared session-state directory for every Tui in one run, so tests can read
-- the .jsonl session logs written under it.
M.STATE_HOME = sys.mkdtemp("/tmp/clm-tui-state-")

-- Control bytes and xterm key sequences, for readable test scripts.
M.CTRL_A = "\1"
M.CTRL_E = "\5"
M.CTRL_K = "\11"
M.CTRL_U = "\21"
M.CTRL_Y = "\25"
M.ESCAPE = "\27"
M.ENTER = "\r"
M.PGUP = "\27[5~"
M.PGDN = "\27[6~"
M.LEFT = "\27[D"
M.RIGHT = "\27[C"
-- Arrows in keypad application mode (SS3), which is what an application that
-- called keypad(TRUE) puts the terminal into. Plain CSI is not what ncurses
-- expects there.
M.UP = "\27OA"
M.DOWN = "\27OB"
M.HOME = "\27OH"
M.END = "\27OF"
-- Bracketed paste (mode 2004): what a terminal wraps pasted text in.
M.PASTE_START = "\27[200~"
M.PASTE_END = "\27[201~"

local Tui = {}
Tui.__index = Tui

-- binary is the clm under test, url the mock server. opts takes rows, cols,
-- args (extra argv) and env (extra environment).
function M.new(binary, url, opts)
	opts = opts or {}
	local rows = opts.rows or 24
	local cols = opts.cols or 80
	local argv = { binary, "--url", url }

	for _, a in ipairs(opts.args or {}) do
		argv[#argv + 1] = a
	end
	local env = {
		TERM = "xterm-256color",
		-- The transcript draws box characters, so the tui needs a
		-- utf-8 locale; a login shell that sets none would give it
		-- the C locale and single-byte output. macOS has no C.UTF-8.
		LC_ALL = sys.os == "darwin" and "en_US.UTF-8" or "C.UTF-8",
		-- The in-tree test config, so agent profiles resolve.
		XDG_CONFIG_HOME = M.TEST_DIR .. "/config",
		-- Session logs and scratch directories go to a per-run temp
		-- dir, never the developer's own state or cache.
		XDG_STATE_HOME = M.STATE_HOME,
		XDG_CACHE_HOME = M.STATE_HOME .. "/cache",
	}
	for k, v in pairs(opts.env or {}) do
		env[k] = v
	end

	local fd, pid = sys.spawn(argv, env, rows, cols)
	local t = setmetatable({
		fd = fd,
		pid = pid,
		rows = rows,
		cols = cols,
		screen = vt.new(rows, cols),
	}, Tui)
	-- The first curses frame arrives immediately. Drain it until the pty
	-- goes quiet rather than sleeping a fixed time per instance.
	t:drain(0.5, 0.03, true)
	return t
end

-- Feed output until the pty has been quiet for `quiet` seconds. `wait` lets
-- process startup take up to timeout; ordinary local operations return after
-- one quiet interval when no redraw was necessary.
function Tui:drain(timeout, quiet, wait)
	timeout = timeout or 0.5
	quiet = quiet or 0.03
	local deadline = sys.now() + timeout
	local received = false

	while sys.now() < deadline do
		local left = deadline - sys.now()
		local interval = (received or not wait) and quiet or left

		if interval > left then
			interval = left
		end
		local data = sys.read(self.fd, math.floor(interval * 1000))
		if data == nil or data == false then
			return
		end
		received = true
		self.screen:feed(data)
	end
end

-- Feed output into the emulator for a fixed window of time.
function Tui:pump(seconds)
	local deadline = sys.now() + seconds

	while sys.now() < deadline do
		local data = sys.read(self.fd, 50)
		if data == false then
			return
		end
		if data ~= nil then
			self.screen:feed(data)
		end
	end
end

function Tui:send(data)
	sys.write(self.fd, data)
end

function Tui:resize(rows, cols)
	self.rows, self.cols = rows, cols
	self.screen:resize(rows, cols)
	sys.winsize(self.fd, rows, cols)
	sys.kill(self.pid, sys.SIGWINCH)
	self:drain()
end

-- Screen rows, trailing blanks removed, one-based.
function Tui:lines()
	local out = {}

	for y = 0, self.rows - 1 do
		out[y + 1] = (self.screen:line(y):gsub("%s+$", ""))
	end
	return out
end

function Tui:text()
	return table.concat(self:lines(), "\n")
end

function Tui:wait_for(needle, timeout)
	local deadline = sys.now() + (timeout or 10.0)

	while sys.now() < deadline do
		self:pump(0.2)
		if self:text():find(needle, 1, true) then
			return true
		end
	end
	return false
end

-- The cell at row y, column x, both zero-based.
function Tui:cell(y, x)
	return self.screen:cell(y, x)
end

-- Where the terminal would draw the caret: row, column, both zero-based.
function Tui:cursor()
	return self.screen:cursor()
end

-- True when every character of needle appears bold somewhere on screen.
function Tui:any_bold(needle)
	local found = {}

	for y = 0, self.rows - 1 do
		for x = 0, self.cols - 1 do
			local c = self.screen:cell(y, x)
			if (c.attr & vt.BOLD) ~= 0 and c.ch:match("%S") then
				found[c.ch] = true
			end
		end
	end
	for ch in needle:gmatch("%S") do
		if not found[ch] then
			return false
		end
	end
	return true
end

function Tui:close()
	if self.pid ~= nil then
		self:send("quit\r")
		-- A normal quit exits promptly. Wait only briefly, then fall
		-- back to a signal for a wedged client.
		local deadline = sys.now() + 0.3
		while sys.now() < deadline do
			if sys.wait(self.pid, true) == self.pid then
				self.pid = nil
				break
			end
			self:drain(0.05, 0.01)
		end
	end
	if self.pid ~= nil then
		sys.kill(self.pid, sys.SIGTERM)
	end
	sys.close(self.fd)
	if self.pid ~= nil then
		sys.wait(self.pid, false)
		self.pid = nil
	end
end

return M
