// SPDX-License-Identifier: ISC
/*
 * Fuzz target for md_render(). Feeds arbitrary bytes into the
 * markdown parser. Looks for crashes, hangs, or memory errors.
 */
#include <stddef.h>
#include <stdint.h>

#include "afl.h"

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

static void
fuzz_one(const uint8_t *data, size_t size)
{
	struct md_opts opts = {.width = 80, .tables = MD_TABLE_PLAIN};

	if (size == 0)
		return;

	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.tables = MD_TABLE_ASCII;
	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.tables = MD_TABLE_UNICODE;
	md_render((const char *)data, size, &opts, sink_emit, NULL);

	opts.width = 1;
	md_render((const char *)data, size, &opts, sink_emit, NULL);
}

__AFL_FUZZ_INIT()

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif

	const uint8_t *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(CLM_FUZZ_LOOPS))
		fuzz_one(buf, (size_t)__AFL_FUZZ_TESTCASE_LEN);

	return 0;
}
