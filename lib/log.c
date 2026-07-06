// SPDX-License-Identifier: ISC
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

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

	/* Timestamp + PID prefix */
	{
		struct timespec ts;
		struct tm tm;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		(void)fprintf(out, "%02d:%02d:%02d.%03ld [%d] ",
		    tm.tm_hour, tm.tm_min, tm.tm_sec,
		    ts.tv_nsec / 1000000, getpid());
	}

	va_start(ap, fmt);
	(void)vfprintf(out, fmt, ap);
	va_end(ap);
	(void)fputc('\n', out);
	(void)fflush(out);
}
