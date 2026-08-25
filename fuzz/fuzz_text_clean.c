// SPDX-License-Identifier: ISC
/*
 * Fuzz target for clm_clean_text(): tool output is whatever a command chose
 * to print, so this sees arbitrary bytes.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "afl.h"

size_t clm_clean_text(uint8_t *data, size_t len);

static void
fuzz_one(const uint8_t *data, size_t size)
{
	uint8_t *buf;
	size_t n;

	if (size == 0)
		return;

	/* A fresh allocation of exactly size bytes, so any read or write
	 * outside the input is a heap error rather than slack in a static
	 * buffer. */
	buf = malloc(size);
	if (buf == NULL)
		return;
	memcpy(buf, data, size);

	n = clm_clean_text(buf, size);
	assert(n <= size);

	/* Idempotent: the output holds nothing left to clean. */
	assert(clm_clean_text(buf, n) == n);

	free(buf);
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
