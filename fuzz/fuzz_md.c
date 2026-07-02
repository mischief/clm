// SPDX-License-Identifier: ISC
/*
 * Fuzz target for md_render(). Feeds arbitrary bytes into the
 * markdown parser. Looks for crashes, hangs, or memory errors.
 *
 * Build: link fuzz_md.c + src/md_render.c + md4c.
 */
#include <stddef.h>
#include <stdint.h>

struct md_run {
	unsigned style;
	const char *text;
	size_t len;
};

enum md_table_style {
	MD_TABLE_AUTO = 0,
	MD_TABLE_UNICODE,
	MD_TABLE_ASCII,
	MD_TABLE_PLAIN,
};

struct md_opts {
	int width;
	enum md_table_style tables;
};

int md_render(const char *md, size_t len, const struct md_opts *opts,
    void (*emit)(const struct md_run *run, void *userdata), void *userdata);

static void
sink_emit(const struct md_run *run, void *userdata)
{
	(void)run;
	(void)userdata;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct md_opts opts = {.width = 80, .tables = MD_TABLE_PLAIN};

	if (size == 0)
		return 0;

	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.tables = MD_TABLE_ASCII;
	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.tables = MD_TABLE_UNICODE;
	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.width = 1;
	md_render((const char *)data, size, &opts, sink_emit, NULL);

	return 0;
}
