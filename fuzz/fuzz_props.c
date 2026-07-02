// SPDX-License-Identifier: ISC
/*
 * Fuzz target for clm_parse_props(). Feeds arbitrary bytes into the
 * /props JSON parser. Looks for crashes, hangs, or unexpected return
 * values on malicious/malformed input.
 *
 * Build: link fuzz_props.c + lib/props.c + json-c.
 */
#include <stdint.h>

int
clm_parse_props(const char *body, int64_t *ctx_out);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int64_t ctx = 0;
	if (size == 0)
		return 0;

	/* NUL-terminate the input (json-c expects strings). */
	/* json_tokener_parse internally stops at NUL, so trailing bytes
	 * after first NUL are irrelevant; just pass directly. */
	clm_parse_props((const char *)data, &ctx);
	return 0;
}
