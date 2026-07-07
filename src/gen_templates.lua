-- Emit a C source embedding *.tpl files as NUL-terminated byte arrays, for
-- `clm setup`'s canned config.lua/secrets.lua. A plain string literal hit
-- ISO C99's mandatory-support minimum for a single string literal (4095
-- bytes -- see -Woverlength-strings, which this project's -Wpedantic
-- -Werror build turns into a hard error) once the provider list grew past
-- a handful of entries; a byte-array initializer has no such limit.

local function ident(name)
	return (name:gsub("[^%w]", "_"))
end

local function basename(path)
	return path:match("([^/]+)$")
end

local out_path = arg[1]
local tpl_paths = {}
for i = 2, #arg do
	tpl_paths[#tpl_paths + 1] = arg[i]
end

local out = assert(io.open(out_path, "w"))
out:write('#include "templates.h"\n\n')

for _, path in ipairs(tpl_paths) do
	local f = assert(io.open(path, "rb"))
	local data = f:read("a")
	f:close()

	local sym = ident(basename(path)) .. "_data"

	out:write("const unsigned char " .. sym .. "[] = {\n")
	for i = 1, #data, 16 do
		local bytes = {}
		for j = i, math.min(i + 15, #data) do
			bytes[#bytes + 1] = tostring(data:byte(j))
		end
		out:write("\t" .. table.concat(bytes, ",") .. ",\n")
	end
	out:write("\t0,\n") -- NUL terminator -- callers use these as plain C strings
	out:write("};\n\n")
end

out:close()
