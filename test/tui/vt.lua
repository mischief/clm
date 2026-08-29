-- SPDX-License-Identifier: ISC
--
-- A small VT100/xterm screen emulator, so a test can read the grid an
-- ncurses application would have painted. It covers what ncurses emits
-- under xterm-256color: cursor motion, erase, scroll region, insert and
-- delete, SGR text attributes. Mouse reports, palette changes and titles
-- are parsed and dropped. Colours are dropped too -- assertions are about
-- layout and emphasis, and every cell carries a text-attribute mask.
local vt = {}

local BOLD = 1
local ITALIC = 2
local UNDERLINE = 4
local BLINK = 8
local REVERSE = 16
local STRIKE = 32

vt.BOLD = BOLD
vt.ITALIC = ITALIC
vt.UNDERLINE = UNDERLINE
vt.BLINK = BLINK
vt.REVERSE = REVERSE
vt.STRIKE = STRIKE

-- SGR parameter -> {bit, on}.
local SGR_TEXT = {
	[1] = { BOLD, true },
	[3] = { ITALIC, true },
	[4] = { UNDERLINE, true },
	[5] = { BLINK, true },
	[7] = { REVERSE, true },
	[9] = { STRIKE, true },
	[22] = { BOLD, false },
	[23] = { ITALIC, false },
	[24] = { UNDERLINE, false },
	[25] = { BLINK, false },
	[27] = { REVERSE, false },
	[29] = { STRIKE, false },
}

-- Modes are tracked by number; private ones are shifted so DECAWM (private
-- 7) never collides with the ANSI mode 7.
local IRM = 4
local DECOM = 6 * 32
local DECAWM = 7 * 32

-- Cells are immutable and interned, so a screen of spaces costs one table.
local cell_cache = {}

local function cell(ch, attr)
	local by_attr = cell_cache[attr]
	if by_attr == nil then
		by_attr = {}
		cell_cache[attr] = by_attr
	end
	local c = by_attr[ch]
	if c == nil then
		c = { ch = ch, attr = attr }
		by_attr[ch] = c
	end
	return c
end

local BLANK = cell(" ", 0)

-- Character width. Everything outside the wide and zero-width ranges is one
-- column, which is all a terminal needs to lay text out.
local WIDE = {
	{ 0x1100, 0x115f },
	{ 0x2e80, 0x303e },
	{ 0x3041, 0x33ff },
	{ 0x3400, 0x4dbf },
	{ 0x4e00, 0x9fff },
	{ 0xa000, 0xa4cf },
	{ 0xac00, 0xd7a3 },
	{ 0xf900, 0xfaff },
	{ 0xfe30, 0xfe6f },
	{ 0xff00, 0xff60 },
	{ 0xffe0, 0xffe6 },
	{ 0x1f300, 0x1f64f },
	{ 0x1f900, 0x1f9ff },
	{ 0x20000, 0x3fffd },
}

local function char_width(cp)
	if cp == 0 then
		return 0
	end
	if cp < 32 or (cp >= 0x7f and cp < 0xa0) then
		return -1
	end
	if (cp >= 0x300 and cp <= 0x36f) or (cp >= 0x200b and cp <= 0x200f) then
		return 0
	end
	for i = 1, #WIDE do
		if cp >= WIDE[i][1] and cp <= WIDE[i][2] then
			return 2
		end
	end
	return 1
end

local Screen = {}
Screen.__index = Screen

-- Rows and cells are sparse: an absent entry reads as a blank with default
-- attributes, which is what "never written" means on a real terminal.
local function row(self, y)
	local r = self.buf[y]
	if r == nil then
		r = {}
		self.buf[y] = r
	end
	return r
end

local function at(self, y, x)
	local r = self.buf[y]
	if r == nil then
		return BLANK
	end
	return r[x] or BLANK
end

function vt.new(rows, cols)
	local self = setmetatable({}, Screen)
	self.rows = rows
	self.cols = cols
	self:reset()
	return self
end

function Screen:reset()
	self.buf = {}
	self.margins = nil
	self.mode = { [DECAWM] = true }
	self.cy, self.cx, self.attr = 0, 0, 0
	self.savepoints = {}
	self.tabstops = {}
	local x = 8
	while x < self.cols do
		self.tabstops[x] = true
		x = x + 8
	end
	self.pending = "" -- undecoded utf-8 tail
	self.state = "ground"
end

function Screen:margin_top()
	return self.margins and self.margins.top or 0
end

function Screen:margin_bottom()
	return self.margins and self.margins.bottom or self.rows - 1
end

local function ensure_h(self)
	if self.cx < 0 then
		self.cx = 0
	elseif self.cx > self.cols - 1 then
		self.cx = self.cols - 1
	end
end

local function ensure_v(self, use_margins)
	local top, bottom = 0, self.rows - 1
	if (use_margins or self.mode[DECOM]) and self.margins then
		top, bottom = self.margins.top, self.margins.bottom
	end
	if self.cy < top then
		self.cy = top
	elseif self.cy > bottom then
		self.cy = bottom
	end
end

-- ---- rendering primitives ----

function Screen:draw_char(ch, cp)
	local w = char_width(cp)

	if self.cx == self.cols then
		if self.mode[DECAWM] then
			self.cx = 0
			self:index()
		elseif w > 0 then
			self.cx = self.cx - w
		end
	end
	if self.mode[IRM] and w > 0 then
		self:insert_characters(w)
	end

	local r = row(self, self.cy)
	if w == 1 then
		r[self.cx] = cell(ch, self.attr)
	elseif w == 2 then
		r[self.cx] = cell(ch, self.attr)
		if self.cx + 1 < self.cols then
			r[self.cx + 1] = cell("", self.attr)
		end
	elseif w == 0 then
		-- A combining mark joins the character before it.
		if self.cx > 0 then
			local prev = r[self.cx - 1] or BLANK
			r[self.cx - 1] = cell(prev.ch .. ch, prev.attr)
		end
	else
		return
	end
	if w > 0 then
		self.cx = math.min(self.cx + w, self.cols)
	end
end

function Screen:index()
	local top, bottom = self:margin_top(), self:margin_bottom()
	if self.cy == bottom then
		for y = top, bottom - 1 do
			self.buf[y] = self.buf[y + 1]
		end
		self.buf[bottom] = nil
	else
		self:cursor_down(1)
	end
end

function Screen:reverse_index()
	local top, bottom = self:margin_top(), self:margin_bottom()
	if self.cy == top then
		for y = bottom, top + 1, -1 do
			self.buf[y] = self.buf[y - 1]
		end
		self.buf[top] = nil
	else
		self:cursor_up(1)
	end
end

function Screen:scroll_up(n)
	local top, bottom = self:margin_top(), self:margin_bottom()
	for _ = 1, n do
		for y = top, bottom - 1 do
			self.buf[y] = self.buf[y + 1]
		end
		self.buf[bottom] = nil
	end
end

function Screen:scroll_down(n)
	local top, bottom = self:margin_top(), self:margin_bottom()
	for _ = 1, n do
		for y = bottom, top + 1, -1 do
			self.buf[y] = self.buf[y - 1]
		end
		self.buf[top] = nil
	end
end

function Screen:cursor_up(n)
	self.cy = math.max(self.cy - (n or 1), self:margin_top())
end

function Screen:cursor_down(n)
	self.cy = math.min(self.cy + (n or 1), self:margin_bottom())
end

function Screen:cursor_position(line, col)
	col = (col or 1) - 1
	line = (line or 1) - 1
	if self.margins and self.mode[DECOM] then
		line = line + self.margins.top
		if line < self.margins.top or line > self.margins.bottom then
			return
		end
	end
	self.cx, self.cy = col, line
	ensure_h(self)
	ensure_v(self)
end

function Screen:erase_in_line(how)
	local first, last
	if how == 1 then
		first, last = 0, self.cx
	elseif how == 2 then
		first, last = 0, self.cols - 1
	else
		first, last = self.cx, self.cols - 1
	end
	local r = row(self, self.cy)
	for x = first, last do
		r[x] = cell(" ", self.attr)
	end
end

function Screen:erase_in_display(how)
	local first, last
	if how == 1 then
		first, last = 0, self.cy - 1
	elseif how == 2 or how == 3 then
		first, last = 0, self.rows - 1
	else
		first, last = self.cy + 1, self.rows - 1
	end
	local blank = cell(" ", self.attr)
	for y = first, last do
		local r = self.buf[y]
		if r ~= nil then
			for x in pairs(r) do
				r[x] = blank
			end
		end
	end
	if how == 0 or how == 1 then
		self:erase_in_line(how)
	end
end

function Screen:insert_characters(n)
	n = n or 1
	local r = row(self, self.cy)
	for x = self.cols, self.cx, -1 do
		if x + n <= self.cols then
			r[x + n] = r[x]
		end
		r[x] = nil
	end
end

function Screen:delete_characters(n)
	n = n or 1
	local r = row(self, self.cy)
	for x = self.cx, self.cols - 1 do
		if x + n <= self.cols then
			r[x] = r[x + n]
			r[x + n] = nil
		else
			r[x] = nil
		end
	end
end

function Screen:erase_characters(n)
	n = n or 1
	local r = row(self, self.cy)
	local blank = cell(" ", self.attr)
	for x = self.cx, math.min(self.cx + n, self.cols) - 1 do
		r[x] = blank
	end
end

function Screen:insert_lines(n)
	n = n or 1
	local top, bottom = self:margin_top(), self:margin_bottom()
	if self.cy < top or self.cy > bottom then
		return
	end
	for y = bottom, self.cy, -1 do
		if y + n <= bottom then
			self.buf[y + n] = self.buf[y]
		end
		self.buf[y] = nil
	end
	self.cx = 0
end

function Screen:delete_lines(n)
	n = n or 1
	local top, bottom = self:margin_top(), self:margin_bottom()
	if self.cy < top or self.cy > bottom then
		return
	end
	for y = self.cy, bottom do
		if y + n <= bottom then
			self.buf[y] = self.buf[y + n]
			self.buf[y + n] = nil
		else
			self.buf[y] = nil
		end
	end
	self.cx = 0
end

function Screen:set_margins(top, bottom)
	if (top == nil or top == 0) and bottom == nil then
		self.margins = nil
		return
	end
	local cur_top = self:margin_top()
	local cur_bottom = self:margin_bottom()
	if top == nil then
		top = cur_top
	else
		top = math.max(0, math.min(top - 1, self.rows - 1))
	end
	if bottom == nil then
		bottom = cur_bottom
	else
		bottom = math.max(0, math.min(bottom - 1, self.rows - 1))
	end
	if bottom - top >= 1 then
		self.margins = { top = top, bottom = bottom }
		self:cursor_position()
	end
end

function Screen:sgr(params)
	if #params == 0 or (#params == 1 and params[1] == 0) then
		self.attr = 0
		return
	end
	local i = 1
	while i <= #params do
		local p = params[i]
		local t = SGR_TEXT[p]
		if p == 0 then
			self.attr = 0
		elseif t then
			if t[2] then
				self.attr = self.attr | t[1]
			else
				self.attr = self.attr & ~t[1]
			end
		elseif p == 38 or p == 48 then
			-- Extended colour: skip its arguments.
			local n = params[i + 1]
			if n == 5 then
				i = i + 2
			elseif n == 2 then
				i = i + 4
			end
		end
		i = i + 1
	end
end

function Screen:set_mode(private, params)
	for _, p in ipairs(params) do
		self.mode[private and p * 32 or p] = true
	end
end

function Screen:reset_mode(private, params)
	for _, p in ipairs(params) do
		self.mode[private and p * 32 or p] = nil
	end
end

function Screen:save_cursor()
	self.savepoints[#self.savepoints + 1] =
	    { cy = self.cy, cx = self.cx, attr = self.attr }
end

function Screen:restore_cursor()
	local sp = table.remove(self.savepoints)
	if sp == nil then
		self.mode[DECOM] = nil
		self:cursor_position()
		return
	end
	self.cy, self.cx, self.attr = sp.cy, sp.cx, sp.attr
	ensure_h(self)
	ensure_v(self, true)
end

function Screen:tab()
	local column = self.cols - 1
	for x = self.cx + 1, self.cols - 1 do
		if self.tabstops[x] then
			column = x
			break
		end
	end
	self.cx = column
end

-- ---- control dispatch ----

local function ctrl_char(self, b)
	if b == 0x08 then -- BS
		if self.cx == self.cols then
			self.cx = self.cx - 1
		end
		self.cx = self.cx - 1
		ensure_h(self)
	elseif b == 0x09 then -- HT
		self:tab()
	elseif b == 0x0a or b == 0x0b or b == 0x0c then -- LF, VT, FF
		self:index()
	elseif b == 0x0d then -- CR
		self.cx = 0
	end
	-- BEL, SO and SI have no effect on the grid.
end

local function is_basic(b)
	return b == 0x07 or (b >= 0x08 and b <= 0x0d) or b == 0x0e or b == 0x0f
end

local function csi_dispatch(self, final, private, params)
	local p1 = params[1]
	if final == "@" then
		self:insert_characters(p1 ~= 0 and p1 or nil)
	elseif final == "A" then
		self:cursor_up(p1 ~= 0 and p1 or 1)
	elseif final == "B" or final == "e" then
		self:cursor_down(p1 ~= 0 and p1 or 1)
	elseif final == "C" or final == "a" then
		self.cx = self.cx + (p1 ~= 0 and p1 or 1)
		ensure_h(self)
	elseif final == "D" then
		if self.cx == self.cols then
			self.cx = self.cx - 1
		end
		self.cx = self.cx - (p1 ~= 0 and p1 or 1)
		ensure_h(self)
	elseif final == "E" then
		self:cursor_down(p1 ~= 0 and p1 or 1)
		self.cx = 0
	elseif final == "F" then
		self:cursor_up(p1 ~= 0 and p1 or 1)
		self.cx = 0
	elseif final == "G" or final == "`" then
		self.cx = (p1 ~= 0 and p1 or 1) - 1
		ensure_h(self)
	elseif final == "H" or final == "f" then
		self:cursor_position(p1 ~= 0 and p1 or nil,
		    (params[2] or 0) ~= 0 and params[2] or nil)
	elseif final == "J" then
		self:erase_in_display(p1)
	elseif final == "K" then
		self:erase_in_line(p1)
	elseif final == "L" then
		self:insert_lines(p1 ~= 0 and p1 or nil)
	elseif final == "M" then
		self:delete_lines(p1 ~= 0 and p1 or nil)
	elseif final == "P" then
		self:delete_characters(p1 ~= 0 and p1 or nil)
	elseif final == "S" then
		self:scroll_up(p1 ~= 0 and p1 or 1)
	elseif final == "T" then
		self:scroll_down(p1 ~= 0 and p1 or 1)
	elseif final == "X" then
		self:erase_characters(p1 ~= 0 and p1 or nil)
	elseif final == "d" then
		self.cy = (p1 ~= 0 and p1 or 1) - 1
		if self.mode[DECOM] and self.margins then
			self.cy = self.cy + self.margins.top
		end
		ensure_v(self)
	elseif final == "g" then
		if p1 == 3 then
			self.tabstops = {}
		else
			self.tabstops[self.cx] = nil
		end
	elseif final == "h" then
		self:set_mode(private, params)
	elseif final == "l" then
		self:reset_mode(private, params)
	elseif final == "m" then
		self:sgr(params)
	elseif final == "r" then
		self:set_margins(p1 ~= 0 and p1 or nil,
		    (params[2] or 0) ~= 0 and params[2] or nil)
	end
	-- Device reports, window operations and palette changes are ignored:
	-- nothing under test reads them back.
end

local function esc_dispatch(self, ch)
	if ch == "c" then
		self:reset()
	elseif ch == "D" then
		self:index()
	elseif ch == "E" then
		self:index()
		self.cx = 0
	elseif ch == "M" then
		self:reverse_index()
	elseif ch == "H" then
		self.tabstops[self.cx] = true
	elseif ch == "7" then
		self:save_cursor()
	elseif ch == "8" then
		self:restore_cursor()
	end
	-- Keypad mode (ESC = / ESC >) changes only what keys the terminal
	-- sends back, not the grid.
end

-- ---- byte feed ----

-- Decode one utf-8 codepoint at s[i]; returns codepoint, byte length. A
-- truncated tail returns nil so the caller can hold it for the next feed.
local function utf8_at(s, i)
	local b = s:byte(i)
	local need, cp
	if b < 0x80 then
		return b, 1
	elseif b >= 0xc2 and b <= 0xdf then
		need, cp = 1, b & 0x1f
	elseif b >= 0xe0 and b <= 0xef then
		need, cp = 2, b & 0x0f
	elseif b >= 0xf0 and b <= 0xf4 then
		need, cp = 3, b & 0x07
	else
		return 0xfffd, 1 -- stray continuation or overlong lead
	end
	if i + need > #s then
		return nil, need + 1
	end
	for k = 1, need do
		local c = s:byte(i + k)
		if c < 0x80 or c > 0xbf then
			return 0xfffd, 1
		end
		cp = (cp << 6) | (c & 0x3f)
	end
	return cp, need + 1
end

function Screen:feed(data)
	local s = self.pending .. data
	self.pending = ""
	local i = 1
	local n = #s
	while i <= n do
		local cp, len = utf8_at(s, i)
		if cp == nil then
			self.pending = s:sub(i)
			return
		end
		local ch = s:sub(i, i + len - 1)
		i = i + len
		self:consume(cp, ch)
	end
end

function Screen:consume(cp, ch)
	local st = self.state
	if st == "ground" then
		if cp == 0x1b then
			self.state = "esc"
		elseif is_basic(cp) then
			ctrl_char(self, cp)
		elseif cp ~= 0 and cp ~= 0x7f then
			self:draw_char(ch, cp)
		end
	elseif st == "esc" then
		if ch == "[" then
			self.state = "csi"
			self.params = {}
			self.current = ""
			self.private = false
		elseif ch == "]" then
			self.state = "osc"
		elseif ch == "#" or ch == "%" or ch == "(" or ch == ")" then
			self.state = "esc_skip"
		else
			self.state = "ground"
			esc_dispatch(self, ch)
		end
	elseif st == "esc_skip" then
		self.state = "ground"
	elseif st == "osc" then
		-- Strings end at BEL or ST; nothing in them touches the grid.
		if cp == 0x07 then
			self.state = "ground"
		elseif cp == 0x1b then
			self.state = "osc_esc"
		end
	elseif st == "osc_esc" then
		self.state = ch == "\\" and "ground" or "osc"
	elseif st == "csi" then
		if ch == "?" then
			self.private = true
		elseif is_basic(cp) then
			ctrl_char(self, cp)
		elseif ch == " " or ch == ">" or ch == "<" or ch == "=" then
			self.private = self.private or ch == "?"
		elseif cp == 0x18 or cp == 0x1a then -- CAN, SUB
			self.state = "ground"
			self:draw_char(ch, cp)
		elseif ch >= "0" and ch <= "9" then
			self.current = self.current .. ch
		elseif ch == "$" then
			self.state = "esc_skip"
		else
			local v = math.tointeger(tonumber(self.current)) or 0
			self.params[#self.params + 1] = math.min(v, 9999)
			if ch == ";" or ch == ":" then
				self.current = ""
			else
				self.state = "ground"
				csi_dispatch(self, ch, self.private, self.params)
			end
		end
	end
end

-- ---- inspection ----

function Screen:resize(rows, cols)
	if rows == self.rows and cols == self.cols then
		return
	end
	if rows < self.rows then
		local drop = self.rows - rows
		local saved_y, saved_x = self.cy, self.cx
		local saved_margins = self.margins
		self.margins = nil
		self.cy, self.cx = 0, 0
		self:delete_lines(drop)
		self.margins = saved_margins
		self.cy, self.cx = saved_y, saved_x
	end
	if cols < self.cols then
		for _, r in pairs(self.buf) do
			for x in pairs(r) do
				if x >= cols then
					r[x] = nil
				end
			end
		end
	end
	self.rows, self.cols = rows, cols
	self.margins = nil
end

-- The character at (y, x), zero-based. Stub cells behind a double-width
-- character read as an empty string.
function Screen:cell(y, x)
	return at(self, y, x)
end

-- One screen row as a string. Double-width characters occupy one position,
-- so a row holding them is shorter than the screen is wide.
function Screen:line(y)
	local out = {}
	local skip = false
	for x = 0, self.cols - 1 do
		if skip then
			skip = false
		else
			local c = at(self, y, x)
			out[#out + 1] = c.ch
			skip = c.ch ~= "" and
			    char_width(utf8.codepoint(c.ch, 1)) == 2
		end
	end
	return table.concat(out)
end

function Screen:display()
	local out = {}
	for y = 0, self.rows - 1 do
		out[y + 1] = self:line(y)
	end
	return out
end

function Screen:cursor()
	return self.cy, self.cx
end

return vt
