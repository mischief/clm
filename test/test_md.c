// SPDX-License-Identifier: ISC
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md_render.h"

/*
 * Local autofree: this test links only md_render (no libclm), so it does not
 * pull in lib/cleanup.h, which would drag in cJSON. Same idiom, no deps.
 */
static void
autofree_fn(void *p)
{
	free(*(void **)p);
}
#define autofree __attribute__((cleanup(autofree_fn)))

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                         \
			failures++;                                            \
		} else {                                                       \
			printf("ok    %s\n", (msg));                           \
		}                                                              \
	} while (0)

/* Collect emitted runs into a flat buffer for inspection. */
struct capture {
	char text[8192];
	size_t len;
	/* Per-run record, parallel arrays kept small and simple. */
	unsigned style[512];
	size_t off[512];
	size_t rlen[512];
	size_t nruns;
};

static void
cap_emit(const struct md_run *run, void *userdata)
{
	struct capture *c = userdata;

	if (c->nruns >= 512 || c->len + run->len + 1 >= sizeof(c->text))
		return;
	c->style[c->nruns] = run->style;
	c->off[c->nruns] = c->len;
	c->rlen[c->nruns] = run->len;
	c->nruns++;
	memcpy(c->text + c->len, run->text, run->len);
	c->len += run->len;
	c->text[c->len] = '\0';
}

static struct capture *
render(const char *md, enum md_table_style tables)
{
	struct md_opts o = {.width = 80, .tables = tables};
	struct capture *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	md_render(md, strlen(md), &o, cap_emit, c);
	return c;
}

/* Does any run whose text contains needle carry all of the given style bits? */
static bool
styled(const struct capture *c, const char *needle, unsigned bits)
{
	size_t nl = strlen(needle);

	for (size_t i = 0; i < c->nruns; i++) {
		if (c->rlen[i] >= nl &&
		    memcmp(c->text + c->off[i], needle, nl) == 0 &&
		    (c->style[i] & bits) == bits)
			return true;
	}
	return false;
}

/* The nth line (0-indexed) of the captured output, copied into out. */
static void
line_at(const struct capture *c, size_t n, char *out, size_t outsz)
{
	size_t line = 0, o = 0;

	out[0] = '\0';
	for (size_t i = 0; i < c->len; i++) {
		if (c->text[i] == '\n') {
			if (line == n)
				return;
			line++;
			o = 0;
			continue;
		}
		if (line == n && o + 1 < outsz) {
			out[o++] = c->text[i];
			out[o] = '\0';
		}
	}
}

static void
test_emphasis(void)
{
	autofree struct capture *c = render("plain **bold** _em_ `code`",
	    MD_TABLE_PLAIN);

	if (c == NULL)
		return;
	CHECK(styled(c, "bold", MD_ST_BOLD), "bold span carries BOLD");
	CHECK(styled(c, "em", MD_ST_ITALIC), "em span carries ITALIC");
	CHECK(styled(c, "code", MD_ST_CODE), "code span carries CODE");
	CHECK(!styled(c, "plain", MD_ST_BOLD), "plain text is not bold");
}

static void
test_nesting(void)
{
	autofree struct capture *c = render("**bold _and italic_**",
	    MD_TABLE_PLAIN);

	if (c == NULL)
		return;
	CHECK(styled(c, "and italic", MD_ST_BOLD | MD_ST_ITALIC),
	    "nested span carries BOLD+ITALIC");
}

static void
test_code_block_literal(void)
{
	/* A '*' inside a fenced block must not become emphasis. */
	autofree struct capture *c = render("```\na * b\n```", MD_TABLE_PLAIN);

	if (c == NULL)
		return;
	CHECK(strstr(c->text, "a * b") != NULL, "code block keeps literal text");
	CHECK(styled(c, "a * b", MD_ST_CODE), "code block text carries CODE");
}

static const char *table_md =
    "| Fruit | Colour |\n"
    "|-------|--------|\n"
    "| Apple | **Red** |\n"
    "| Fig | Purple |\n";

static void
test_table_alignment(void)
{
	autofree struct capture *c = render(table_md, MD_TABLE_ASCII);
	char l0[128], l2[128];

	if (c == NULL)
		return;
	/* Row 0 is the header, row 1 the separator rule, row 2 a body row.
	 * Columns are padded to a common width, so rows share a display width. */
	line_at(c, 0, l0, sizeof(l0));
	line_at(c, 2, l2, sizeof(l2));
	CHECK(l0[0] != '\0' && l2[0] != '\0', "table produced header and body");
	CHECK(md_display_width(l0, strlen(l0)) ==
	    md_display_width(l2, strlen(l2)),
	    "table columns aligned to equal width");
}

static void
test_table_bold_cell(void)
{
	autofree struct capture *c = render(table_md, MD_TABLE_ASCII);

	if (c == NULL)
		return;
	CHECK(styled(c, "Red", MD_ST_BOLD), "bold inside a table cell stays bold");
}

static void
test_table_glyphs(void)
{
	autofree struct capture *ascii = render(table_md, MD_TABLE_ASCII);
	autofree struct capture *uni = render(table_md, MD_TABLE_UNICODE);

	if (ascii == NULL || uni == NULL)
		return;
	CHECK(strchr(ascii->text, '|') != NULL, "ascii table uses '|' rule");
	CHECK(strstr(ascii->text, "\u2502") == NULL,
	    "ascii table has no unicode box glyph");
	CHECK(strstr(uni->text, "\u2502") != NULL,
	    "unicode table uses box-drawing rule");
	CHECK(strstr(uni->text, "\u250c") != NULL &&
	    strstr(uni->text, "\u2518") != NULL,
	    "unicode table has corner glyphs");
}

int
main(void)
{
	/* Table alignment measures display width via wcwidth; needs a locale. */
	setlocale(LC_ALL, "");

	test_emphasis();
	test_nesting();
	test_code_block_literal();
	test_table_alignment();
	test_table_bold_cell();
	test_table_glyphs();

	if (failures > 0) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all md_render tests passed\n");
	return 0;
}
