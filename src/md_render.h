// SPDX-License-Identifier: ISC
/*
 * md_render -- markdown to styled terminal runs.
 *
 * A small, self-contained wrapper over md4c that turns a markdown document
 * into a stream of styled text "runs" -- (style bitmask, text) pairs, plus
 * newlines -- suitable for painting into any attribute-capable surface. It is
 * deliberately free of ncurses (or any UI) so it can be unit-tested in
 * isolation: styles are abstract bits (MD_ST_*), and the caller maps them to
 * whatever its display layer uses (e.g. curses A_BOLD).
 *
 * Tables are buffered and laid out with columns aligned to display width
 * (wcwidth-based, locale-aware), so callers get readable tables without doing
 * layout themselves. Requires the caller to have set a UTF-8 locale
 * (setlocale(LC_ALL, "")) for correct width measurement and box-glyph
 * autodetection.
 */
#ifndef CLM_MD_RENDER_H
#define CLM_MD_RENDER_H

#include <stddef.h>

/* Abstract style flags, OR-combined. Mapped by the caller to display attrs. */
enum md_style {
	MD_ST_BOLD = 1u << 0,
	MD_ST_ITALIC = 1u << 1,
	MD_ST_UNDERLINE = 1u << 2,
	MD_ST_CODE = 1u << 3, /* inline code / code block */
	MD_ST_DIM = 1u << 4,
	MD_ST_STRIKE = 1u << 5,
};

/* One rendered run. text is NOT NUL-terminated; use len. A run of a single
 * '\n' (len 1) denotes a hard line break in the output. */
struct md_run {
	unsigned style;
	const char *text;
	size_t len;
};

/* How to draw table rules/borders. AUTO picks UNICODE on a UTF-8 locale,
 * ASCII otherwise. PLAIN aligns columns but draws no rules. */
enum md_table_style {
	MD_TABLE_AUTO = 0,
	MD_TABLE_UNICODE,
	MD_TABLE_ASCII,
	MD_TABLE_PLAIN,
};

struct md_opts {
	int width;                   /* wrap/layout width in columns; <=0 = 80 */
	enum md_table_style tables;  /* table rule style */
};

/*
 * Parse [md, md+len) and emit runs in document order via emit(). userdata is
 * passed through untouched. Returns 0 on success. On parse failure the whole
 * input is emitted as a single unstyled run (so callers always get output).
 * The pointers in each md_run are valid only for the duration of the emit
 * call; copy what you need.
 */
int md_render(const char *md, size_t len, const struct md_opts *opts,
    void (*emit)(const struct md_run *run, void *userdata), void *userdata);

/* Display width (columns) of the first n bytes of a UTF-8 string, using
 * wcwidth(3). Exposed for callers that also measure text (e.g. wrapping). */
int md_display_width(const char *s, size_t n);

#endif /* CLM_MD_RENDER_H */
