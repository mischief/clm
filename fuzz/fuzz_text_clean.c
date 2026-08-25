// SPDX-License-Identifier: ISC
/*
 * Fuzz target for clm_clean_text(): tool output is whatever a command chose
 * to print, so this sees arbitrary bytes. Build with libclm/text_clean.c.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

size_t clm_clean_text(uint8_t *data, size_t len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *buf;
	size_t n;

	if (size == 0)
		return 0;

	/* A fresh allocation of exactly size bytes, so any read or write
	 * outside the input is a heap error rather than slack in a static
	 * buffer. */
	buf = malloc(size);
	if (buf == NULL)
		return 0;
	memcpy(buf, data, size);

	n = clm_clean_text(buf, size);
	assert(n <= size);

	/* Idempotent: the output holds nothing left to clean. */
	assert(clm_clean_text(buf, n) == n);

	free(buf);
	return 0;
}
