// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <langinfo.h>

#include <md4c.h>

#include "md_render.h"

#include "useful.h"
#include "banned.h"

int
md_display_width(const char *s, size_t n)
{
	mbstate_t ps;
	size_t i = 0;
	int cols = 0;

	ASSERT_RETURN(s != NULL, 0);

	memset(&ps, 0, sizeof(ps));
	while (i < n) {
		wchar_t wc;
		size_t k = mbrtowc(&wc, s + i, n - i, &ps);
		if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
			i++; /* invalid/incomplete: count as one column */
			cols++;
			continue;
		}
		int w = wcwidth(wc);
		cols += (w > 0) ? w : (w == 0 ? 0 : 1);
		i += k;
	}
	return cols;
}

/* ---- a growable buffer of styled runs, used for table cells ---- */

struct run {
	unsigned style;
	char *text; /* owned, NUL-terminated */
	size_t len;
};

struct runbuf {
	struct run *v;
	size_t n, cap;
};

static int
runbuf_push(struct runbuf *b, unsigned style, const char *text, size_t len)
{
	if (len == 0)
		return 0;
	if (b->n == b->cap) {
		size_t ncap = b->cap ? b->cap * 2 : 8;
		struct run *p = realloc(b->v, ncap * sizeof(*p));
		if (p == NULL)
			return -1;
		b->v = p;
		b->cap = ncap;
	}
	char *dup = malloc(len + 1);
	if (dup == NULL)
		return -1;
	memcpy(dup, text, len);
	dup[len] = '\0';
	b->v[b->n].style = style;
	b->v[b->n].text = dup;
	b->v[b->n].len = len;
	b->n++;
	return 0;
}

static void
runbuf_free(struct runbuf *b)
{
	for (size_t i = 0; i < b->n; i++)
		free(b->v[i].text);
	free(b->v);
	b->v = NULL;
	b->n = b->cap = 0;
}

static int
runbuf_width(const struct runbuf *b)
{
	int w = 0;
	for (size_t i = 0; i < b->n; i++)
		w += md_display_width(b->v[i].text, b->v[i].len);
	return w;
}

/* ---- table accumulator ---- */

struct table {
	struct runbuf *cells; /* row-major: rows*cols */
	unsigned *aligns;     /* per-cell MD_ALIGN */
	size_t rows, cols;
	size_t cap_cells;
	size_t cur_row, cur_col; /* fill cursor */
	size_t head_rows;        /* number of leading header rows */
	bool in_head;
};

/* ---- render context ---- */

struct ctx {
	const struct md_opts *opts;
	void (*emit)(const struct md_run *, void *);
	void *user;

	unsigned attr; /* accumulated span styles */
	int blanks;    /* pending blank lines (coalesced) */
	bool bol;      /* at beginning of a line */

	struct table *tbl; /* non-NULL while inside a table */

	int width;
	enum md_table_style tstyle;
};

static void
emit_run(struct ctx *c, unsigned style, const char *text, size_t len)
{
	struct md_run r = {.style = style, .text = text, .len = len};
	if (len == 0)
		return;
	c->emit(&r, c->user);
}

static void
emit_nl(struct ctx *c)
{
	emit_run(c, 0, "\n", 1);
	c->bol = true;
}

static void
flush_blanks(struct ctx *c)
{
	if (c->blanks > 0 && !c->bol)
		emit_nl(c);
	c->blanks = 0;
}

/* ---- table layout ---- */

static struct runbuf *
tbl_cell(struct table *t, size_t row, size_t col)
{
	return &t->cells[row * t->cols + col];
}

/* Box-drawing glyphs for one table style. Each column occupies w[j]+2 cells
 * (a space of padding on each side of the content); rules fill those same
 * spans so junctions line up under the vertical separators. */
struct box {
	const char *v; /* vertical separator */
	const char *h; /* horizontal fill */
	const char *tl, *tm, *tr; /* top: left, mid tee, right */
	const char *ml, *mm, *mr; /* header sep: left tee, cross, right tee */
	const char *bl, *bm, *br; /* bottom: left, mid tee, right */
};

static const struct box box_unicode = {
	"\u2502", "\u2500",
	"\u250c", "\u252c", "\u2510",
	"\u251c", "\u253c", "\u2524",
	"\u2514", "\u2534", "\u2518",
};

static const struct box box_ascii = {
	"|", "-",
	"+", "+", "+",
	"+", "+", "+",
	"+", "+", "+",
};

static void
emit_str(struct ctx *c, const char *s)
{
	emit_run(c, 0, s, strlen(s));
}

static void
emit_pad(struct ctx *c, int n)
{
	while (n-- > 0)
		emit_run(c, 0, " ", 1);
}

/* A horizontal border: left corner, per-column fill of w[j]+2 glyphs, a
 * junction between columns, and a right corner -- matching the data-row
 * geometry so corners and tees align with the vertical separators. */
static void
emit_border(struct ctx *c, const struct box *b, const int *w, size_t cols,
    const char *left, const char *mid, const char *right)
{
	emit_str(c, left);
	for (size_t j = 0; j < cols; j++) {
		for (int k = 0; k < w[j] + 2; k++)
			emit_str(c, b->h);
		emit_str(c, (j + 1 < cols) ? mid : right);
	}
	emit_nl(c);
}

/* One data row: an edge/separator plus a space-padded, aligned cell in each
 * w[j]+2 span. Header cells are bolded. */
static void
emit_row(struct ctx *c, struct table *t, size_t i, const struct box *b,
    const int *w, bool ruled)
{
	if (ruled)
		emit_str(c, b->v);
	for (size_t j = 0; j < t->cols; j++) {
		struct runbuf *cell = tbl_cell(t, i, j);
		int cw = runbuf_width(cell);
		unsigned al = t->aligns[i * t->cols + j];
		int padtot = w[j] - cw;
		int lpad = 0, rpad = padtot;

		if (al == MD_ALIGN_RIGHT) {
			lpad = padtot;
			rpad = 0;
		} else if (al == MD_ALIGN_CENTER) {
			lpad = padtot / 2;
			rpad = padtot - lpad;
		}

		emit_pad(c, 1 + lpad);
		for (size_t r = 0; r < cell->n; r++) {
			unsigned st = cell->v[r].style;
			if (i < t->head_rows)
				st |= MD_ST_BOLD;
			emit_run(c, st, cell->v[r].text, cell->v[r].len);
		}
		c->bol = false;
		emit_pad(c, rpad + 1);
		if (ruled)
			emit_str(c, b->v);
		else if (j + 1 < t->cols)
			emit_str(c, " ");
	}
	emit_nl(c);
}

static void
layout_table(struct ctx *c, struct table *t)
{
	const struct box *b;
	bool ruled;
	int *w;

	if (t->rows == 0 || t->cols == 0)
		return;

	w = calloc(t->cols, sizeof(*w));
	if (w == NULL)
		return;

	/* Column width = max display width of any cell in the column. */
	for (size_t i = 0; i < t->rows; i++) {
		for (size_t j = 0; j < t->cols; j++) {
			int cw = runbuf_width(tbl_cell(t, i, j));
			if (cw > w[j])
				w[j] = cw;
		}
	}

	ruled = (c->tstyle != MD_TABLE_PLAIN);
	b = (c->tstyle == MD_TABLE_ASCII) ? &box_ascii : &box_unicode;

	flush_blanks(c);

	if (ruled)
		emit_border(c, b, w, t->cols, b->tl, b->tm, b->tr);

	for (size_t i = 0; i < t->rows; i++) {
		emit_row(c, t, i, b, w, ruled);
		if (ruled && t->head_rows > 0 && i + 1 == t->head_rows)
			emit_border(c, b, w, t->cols, b->ml, b->mm, b->mr);
	}

	if (ruled)
		emit_border(c, b, w, t->cols, b->bl, b->bm, b->br);

	c->blanks = 1;
	free(w);
}

static void
table_free(struct table *t)
{
	if (t == NULL)
		return;
	if (t->cells != NULL) {
		size_t total = t->rows * t->cols;
		for (size_t i = 0; i < total; i++)
			runbuf_free(&t->cells[i]);
		free(t->cells);
	}
	free(t->aligns);
	free(t);
}

/* Allocate the cell grid once we know rows*cols from MD_BLOCK_TABLE_DETAIL. */
static int
table_alloc(struct table *t, size_t rows, size_t cols)
{
	size_t total = rows * cols;
	t->cells = calloc(total ? total : 1, sizeof(*t->cells));
	t->aligns = calloc(total ? total : 1, sizeof(*t->aligns));
	if (t->cells == NULL || t->aligns == NULL)
		return -1;
	t->rows = rows;
	t->cols = cols;
	t->cap_cells = total;
	return 0;
}

/* ---- md4c callbacks ---- */

static void
style_on(struct ctx *c, MD_SPANTYPE type)
{
	switch (type) {
	case MD_SPAN_STRONG:
		c->attr |= MD_ST_BOLD;
		break;
	case MD_SPAN_EM:
		c->attr |= MD_ST_ITALIC;
		break;
	case MD_SPAN_U:
		c->attr |= MD_ST_UNDERLINE;
		break;
	case MD_SPAN_CODE:
		c->attr |= MD_ST_CODE;
		break;
	case MD_SPAN_DEL:
		c->attr |= MD_ST_STRIKE;
		break;
	default:
		break;
	}
}

static void
style_off(struct ctx *c, MD_SPANTYPE type)
{
	switch (type) {
	case MD_SPAN_STRONG:
		c->attr &= ~(unsigned)MD_ST_BOLD;
		break;
	case MD_SPAN_EM:
		c->attr &= ~(unsigned)MD_ST_ITALIC;
		break;
	case MD_SPAN_U:
		c->attr &= ~(unsigned)MD_ST_UNDERLINE;
		break;
	case MD_SPAN_CODE:
		c->attr &= ~(unsigned)MD_ST_CODE;
		break;
	case MD_SPAN_DEL:
		c->attr &= ~(unsigned)MD_ST_STRIKE;
		break;
	default:
		break;
	}
}

static int
cb_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	struct ctx *c = userdata;

	switch (type) {
	case MD_BLOCK_DOC:
		break;
	case MD_BLOCK_TABLE: {
		MD_BLOCK_TABLE_DETAIL *d = detail;
		struct table *t = calloc(1, sizeof(*t));
		if (t == NULL)
			return -1;
		size_t rows = d->head_row_count + d->body_row_count;
		if (table_alloc(t, rows, d->col_count) < 0) {
			table_free(t);
			return -1;
		}
		t->head_rows = d->head_row_count;
		c->tbl = t;
		break;
	}
	case MD_BLOCK_THEAD:
		if (c->tbl != NULL)
			c->tbl->in_head = true;
		break;
	case MD_BLOCK_TR:
		if (c->tbl != NULL)
			c->tbl->cur_col = 0;
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		if (c->tbl != NULL && detail != NULL) {
			MD_BLOCK_TD_DETAIL *d = detail;
			size_t idx = c->tbl->cur_row * c->tbl->cols +
			    c->tbl->cur_col;
			if (idx < c->tbl->cap_cells)
				c->tbl->aligns[idx] = (unsigned)d->align;
		}
		break;
	case MD_BLOCK_CODE:
		flush_blanks(c);
		c->attr |= MD_ST_CODE | MD_ST_DIM;
		break;
	case MD_BLOCK_H:
		flush_blanks(c);
		c->attr |= MD_ST_BOLD;
		break;
	case MD_BLOCK_LI:
		flush_blanks(c);
		emit_run(c, 0, "  - ", 4);
		c->bol = false;
		break;
	default:
		flush_blanks(c);
		break;
	}
	return 0;
}

static int
cb_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	struct ctx *c = userdata;
	(void)detail;

	switch (type) {
	case MD_BLOCK_DOC:
		break;
	case MD_BLOCK_TABLE:
		if (c->tbl != NULL) {
			layout_table(c, c->tbl);
			table_free(c->tbl);
			c->tbl = NULL;
		}
		break;
	case MD_BLOCK_THEAD:
		if (c->tbl != NULL)
			c->tbl->in_head = false;
		break;
	case MD_BLOCK_TR:
		if (c->tbl != NULL)
			c->tbl->cur_row++;
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		if (c->tbl != NULL)
			c->tbl->cur_col++;
		break;
	case MD_BLOCK_CODE:
		c->attr &= ~(unsigned)(MD_ST_CODE | MD_ST_DIM);
		if (!c->bol)
			emit_nl(c);
		c->blanks = 1;
		break;
	case MD_BLOCK_H:
		c->attr &= ~(unsigned)MD_ST_BOLD;
		emit_nl(c);
		c->blanks = 1;
		break;
	case MD_BLOCK_LI:
		if (!c->bol)
			emit_nl(c);
		break;
	default:
		if (!c->bol)
			emit_nl(c);
		c->blanks = 1;
		break;
	}
	return 0;
}

static int
cb_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	(void)detail;
	style_on(userdata, type);
	return 0;
}

static int
cb_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	(void)detail;
	style_off(userdata, type);
	return 0;
}

static int
cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
	struct ctx *c = userdata;

	/* Inside a table: buffer text into the current cell instead of emitting.
	 * Cells are single-line, so breaks become spaces. */
	if (c->tbl != NULL) {
		struct table *t = c->tbl;
		size_t idx = t->cur_row * t->cols + t->cur_col;
		if (idx >= t->cap_cells)
			return 0;
		if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR)
			return runbuf_push(&t->cells[idx], c->attr, " ", 1) < 0
			    ? -1 : 0;
		if (type == MD_TEXT_NULLCHAR || size == 0)
			return 0;
		return runbuf_push(&t->cells[idx], c->attr, text, size) < 0
		    ? -1 : 0;
	}

	switch (type) {
	case MD_TEXT_BR:
	case MD_TEXT_SOFTBR:
		emit_nl(c);
		return 0;
	case MD_TEXT_NULLCHAR:
		return 0;
	default:
		break;
	}
	emit_run(c, c->attr, text, size);
	if (size > 0)
		c->bol = false;
	return 0;
}

static enum md_table_style
resolve_table_style(enum md_table_style s)
{
	if (s != MD_TABLE_AUTO)
		return s;
	const char *cs = nl_langinfo(CODESET);
	if (cs != NULL && (strcmp(cs, "UTF-8") == 0 || strcmp(cs, "UTF8") == 0))
		return MD_TABLE_UNICODE;
	return MD_TABLE_ASCII;
}

int
md_render(const char *md, size_t len, const struct md_opts *opts,
    void (*emit)(const struct md_run *, void *), void *userdata)
{
	struct ctx c = {0};
	MD_PARSER parser = {
		.abi_version = 0,
		.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		    MD_FLAG_TASKLISTS | MD_FLAG_NOHTML,
		.enter_block = cb_enter_block,
		.leave_block = cb_leave_block,
		.enter_span = cb_enter_span,
		.leave_span = cb_leave_span,
		.text = cb_text,
		.debug_log = NULL,
		.syntax = NULL,
	};

	ASSERT_RETURN(md != NULL, -EINVAL);
	ASSERT_RETURN(emit != NULL, -EINVAL);

	c.opts = opts;
	c.emit = emit;
	c.user = userdata;
	c.bol = true;
	c.width = (opts != NULL && opts->width > 0) ? opts->width : 80;
	c.tstyle = resolve_table_style(opts != NULL ? opts->tables
	                                             : MD_TABLE_AUTO);

	if (md_parse(md, (MD_SIZE)len, &parser, &c) != 0) {
		table_free(c.tbl);
		c.tbl = NULL;
		emit_run(&c, 0, md, len); /* fallback: raw text */
		return -1;
	}
	table_free(c.tbl); /* defensive: unclosed table on malformed input */
	return 0;
}
