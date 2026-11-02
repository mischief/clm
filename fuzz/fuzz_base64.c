// SPDX-License-Identifier: ISC
/*
 * Fuzz target for clm_base64_decode(): an MCP server sends image data as
 * base64, so this sees arbitrary bytes. The oracle is the encoder: input
 * decodes only if it is the one encoding of its output.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "afl.h"

size_t clm_base64_len(size_t n);
void clm_base64_encode(const uint8_t *in, size_t n, char *out);
int clm_base64_decode(const char *in, size_t n, uint8_t *out, size_t *outlen);

static void
fuzz_one(const uint8_t *data, size_t size)
{
	char *in, *enc;
	uint8_t *out;
	size_t len = 0;

	/* Heap copies one byte past the need, so ASan catches any overrun past
	 * that. The extra byte keeps malloc(0) out. */
	in = malloc(size + 1);
	out = malloc(size / 4 * 3 + 1);
	if (in == NULL || out == NULL)
		goto done;
	memcpy(in, data, size);
	if (clm_base64_decode(in, size, out, &len) != 0)
		goto done;
	assert(len <= size / 4 * 3);
	assert(clm_base64_len(len) == size);
	enc = malloc(size + 1);
	if (enc == NULL)
		goto done;
	clm_base64_encode(out, len, enc);
	assert(memcmp(enc, in, size) == 0);
	free(enc);
done:
	free(out);
	free(in);
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
