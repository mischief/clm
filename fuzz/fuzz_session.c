// SPDX-License-Identifier: ISC
/*
 * Fuzz target for the session JSONL loader: feeds arbitrary bytes to
 * session_parse_line() one input at a time (each fuzz input is treated
 * as one log line) plus, when the input contains newlines, as a whole
 * multi-line log split the way clm_session_load() would see it. The
 * loader's contract is that NO byte sequence is fatal -- bad lines are
 * skipped -- so any crash/leak/UB here is a real bug.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "clm/history.h"
#include "session_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct clm_history h;
	cJSON *meta = NULL;

	clm_history_init(&h);
	(void)session_parse_line(&h, (const char *)data, size, &meta);

	/* Split on newlines like the real loader. */
	const char *p = (const char *)data, *end = p + size;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
		(void)session_parse_line(&h, p, len, &meta);
		p += len + 1;
	}

	if (meta != NULL)
		cJSON_Delete(meta);
	clm_history_free(&h);
	return 0;
}
