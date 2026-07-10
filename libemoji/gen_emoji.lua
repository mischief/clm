-- Emit a C source embedding emoji_data.tsv (shortcode name -> codepoint) as
-- a compact, relocation-free lookup table for the tui input line's
-- :shortcode: expansion/completion.
--
-- Layout, chosen to avoid PIE's per-pointer load-time relocations (see the
-- clm_emoji_table char* version this replaced -- 1410 entries * 2 pointers
-- meant 2820 R_X86_64_RELATIVE fixups at every process start):
--   - clm_emoji_names: one NUL-separated blob of every shortcode name.
--   - clm_emoji_table: { uint16_t name_off; uint16_t cp_packed } per entry,
--     sorted by name for binary search. Plain integers, not pointers -- no
--     relocations at all.
--
-- cp_packed exploits a gap in this dataset: every single-codepoint emoji
-- shortcode's codepoint is either a BMP misc symbol (< 0x8000) or lands in
-- supplementary plane 1 (0x10000-0x1ffff, and in practice 0x1f004-0x1faf8),
-- whose low 16 bits (cp & 0xffff) are always >= 0x8000 for this data. So
-- cp_packed = cp & 0xffff loses no information: on decode, >= 0x8000 means
-- "OR in 0x10000", otherwise it's the literal BMP codepoint. The asserts
-- below make sure that gap still holds if emoji_data.tsv is regenerated
-- from a newer Unicode release -- if it stops holding, this needs a real
-- flag bit (or just go back to storing cp as uint32_t; still relocation-
-- free, just 2 bytes bigger per entry).

local out_path = arg[1]
local in_path = arg[2]

local f = assert(io.open(in_path, "r"))
local entries = {}
for line in f:lines() do
	local name, hexes = line:match("^([^\t]+)\t(.+)$")
	if name then
		local cps = {}
		for hex in hexes:gmatch("[0-9a-fA-F]+") do
			cps[#cps + 1] = hex
		end
		-- Skip multi-codepoint sequences (ZWJ combos, keycaps, skin-tone
		-- modifiers, etc): this system's fonts have no ligature/combining
		-- support, so they'd render as separate glyphs side by side
		-- instead of the intended single emoji.
		if #cps == 1 then
			local cp = tonumber(cps[1], 16)
			if cp < 0x10000 then
				assert(cp < 0x8000,
				    "BMP codepoint " .. cps[1] .. " (" .. name ..
				    ") >= 0x8000 breaks the cp_packed encoding")
			else
				assert(cp >= 0x10000 and cp <= 0x1ffff,
				    "codepoint " .. cps[1] .. " (" .. name ..
				    ") outside supplementary plane 1, " ..
				    "cp_packed can't represent it")
				assert((cp & 0xffff) >= 0x8000,
				    "codepoint " .. cps[1] .. " (" .. name ..
				    ") has low 16 bits < 0x8000, collides with " ..
				    "the BMP range in cp_packed")
			end
			entries[#entries + 1] = { name = name, cp = cp }
		end
	end
end
f:close()

table.sort(entries, function(a, b) return a.name < b.name end)

-- Build the name blob and record each entry's offset into it.
local blob_parts = {}
local blob_len = 0
for _, e in ipairs(entries) do
	assert(blob_len <= 0xffff,
	    "name blob exceeds 65535 bytes, name_off no longer fits uint16_t")
	e.name_off = blob_len
	blob_parts[#blob_parts + 1] = e.name
	blob_len = blob_len + #e.name + 1 -- +1 for the NUL
end
local blob = table.concat(blob_parts, "\0") .. "\0"

local out = assert(io.open(out_path, "w"))
out:write('#include "emoji.h"\n\n')

-- Byte-array initializer, not a string literal: a name blob has NULs
-- immediately followed by digits ("100", "1234", ...), which a literal
-- "\0" escape would misparse as a longer octal escape. See
-- gen_templates.lua for the same trick used for the .tpl blobs.
out:write("const unsigned char clm_emoji_names[] = {\n")
for i = 1, #blob, 16 do
	local bytes = {}
	for j = i, math.min(i + 15, #blob) do
		bytes[#bytes + 1] = tostring(blob:byte(j))
	end
	out:write("\t" .. table.concat(bytes, ",") .. ",\n")
end
out:write("};\n\n")

out:write("const struct clm_emoji_entry clm_emoji_table[] = {\n")
for _, e in ipairs(entries) do
	local cp_packed = e.cp < 0x10000 and e.cp or (e.cp & 0xffff)
	out:write(string.format("\t{ %d, %d },\n", e.name_off, cp_packed))
end
out:write("};\n\n")
out:write(string.format(
    "const size_t clm_emoji_table_len = %d;\n", #entries))
out:close()
