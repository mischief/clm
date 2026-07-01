// SPDX-License-Identifier: ISC
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "clm/log.h"
#include "banned.h"

void
clm_debug(const char *fmt, ...)
{
	static FILE *out;
	static bool tried;
	va_list ap;

	if (out == NULL) {
		const char *path;

		if (tried)
			return;
		tried = true;

		path = getenv("CLM_DEBUG_LOG");
		if (path == NULL)
			return; /* logging disabled unless explicitly enabled */

		out = fopen(path, "ae");
		if (out == NULL)
			return;
	}

	va_start(ap, fmt);
	(void)vfprintf(out, fmt, ap);
	va_end(ap);
	(void)fputc('\n', out);
	(void)fflush(out);
}
