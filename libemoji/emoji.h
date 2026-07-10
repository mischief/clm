#ifndef CLM_EMOJI_H
#define CLM_EMOJI_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* Generated at build time from emoji_data.tsv by gen_emoji.lua into
 * emoji_data.c. See that script's header comment for the cp_packed
 * encoding this relies on -- deliberately relocation-free (no pointers):
 * name_off indexes into clm_emoji_names, a single NUL-separated blob. */
struct clm_emoji_entry {
	uint16_t name_off;
	uint16_t cp_packed;
};

extern const unsigned char clm_emoji_names[];
extern const struct clm_emoji_entry clm_emoji_table[]; /* sorted by name */
extern const size_t clm_emoji_table_len;

/* Look up a shortcode by name (not including the surrounding colons;
 * namelen bytes, need not be NUL-terminated). On a match, encodes the
 * UTF-8 glyph into out (must be at least 5 bytes: up to 4 bytes plus a
 * NUL) and returns the encoded length (1-4). Returns 0 if unknown. */
size_t clm_emoji_lookup(const char *name, size_t namelen, char out[5]);

/* i must be < clm_emoji_table_len; "" on violation in release builds, not
 * NULL, so it stays safe to strlen/strncmp. */
static inline const char *
clm_emoji_entry_name(size_t i)
{
	assert(i < clm_emoji_table_len);
	if (i >= clm_emoji_table_len)
		return "";
	return (const char *)clm_emoji_names + clm_emoji_table[i].name_off;
}

#endif
