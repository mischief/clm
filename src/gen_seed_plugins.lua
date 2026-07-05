-- Emit a C source embedding *.lua files as byte arrays, for seeding a
-- user's plugin directory on first run of a static/standalone build.

local function ident(name)
	return (name:gsub("[^%w]", "_"))
end

local function basename(path)
	return path:match("([^/]+)$")
end

local out_path = arg[1]
local lua_paths = {}
for i = 2, #arg do
	lua_paths[#lua_paths + 1] = arg[i]
end

local out = assert(io.open(out_path, "w"))
out:write('#include "seed_plugins.h"\n\n')

local syms = {}
for _, path in ipairs(lua_paths) do
	local f = assert(io.open(path, "rb"))
	local data = f:read("a")
	f:close()

	local name = basename(path)
	local sym = "seed_" .. ident(name)
	syms[#syms + 1] = { name = name, sym = sym }

	out:write("static const unsigned char " .. sym .. "[] = {\n")
	for i = 1, #data, 16 do
		local bytes = {}
		for j = i, math.min(i + 15, #data) do
			bytes[#bytes + 1] = tostring(data:byte(j))
		end
		out:write("\t" .. table.concat(bytes, ",") .. ",\n")
	end
	out:write("};\n\n")
end

out:write("const struct clm_seed_plugin clm_seed_plugins[] = {\n")
for _, s in ipairs(syms) do
	out:write(string.format('\t{ "%s", %s, sizeof(%s) },\n', s.name, s.sym, s.sym))
end
out:write("\t{ NULL, NULL, 0 },\n")
out:write("};\n")

out:close()
