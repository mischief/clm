#include "emoji.h"

#include <string.h>

static size_t
put_utf8(unsigned long cp, char out[5])
{
	if (cp <= 0x7f) {
		out[0] = (char)cp;
		return 1;
	} else if (cp <= 0x7ff) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	} else if (cp <= 0xffff) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	} else {
		out[0] = (char)(0xf0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[3] = (char)(0x80 | (cp & 0x3f));
		return 4;
	}
}

/* Undo gen_emoji.lua's cp_packed trick: >= 0x8000 means "in supplementary
 * plane 1", OR in the 0x10000 that got masked off; otherwise it's a
 * literal BMP codepoint (this dataset never uses BMP codepoints >= 0x8000,
 * see the generator's asserts). */
static unsigned long
unpack_codepoint(uint16_t cp_packed)
{
	return cp_packed >= 0x8000 ? (0x10000ul | cp_packed) : cp_packed;
}

size_t
clm_emoji_lookup(const char *name, size_t namelen, char out[5])
{
	size_t lo = 0, hi = clm_emoji_table_len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const char *cand = clm_emoji_entry_name(mid);
		size_t candlen = strlen(cand);
		size_t cmplen = namelen < candlen ? namelen : candlen;
		int cmp = memcmp(name, cand, cmplen);
		if (cmp == 0)
			cmp = (int)namelen - (int)candlen;

		if (cmp == 0) {
			size_t n = put_utf8(
			    unpack_codepoint(clm_emoji_table[mid].cp_packed),
			    out);
			out[n] = '\0';
			return n;
		} else if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return 0;
}
