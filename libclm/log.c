// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clm/log.h"
#include "banned.h"

CLM_API void
clm_debug(const char *fmt, ...)
{
	static int fd = -1;
	static bool tried;
	va_list ap, ap2;
	struct timespec ts;
	struct tm tm;
	char head[64];
	char *line;
	int hn, mn;
	size_t total, off;

	if (fd < 0) {
		const char *path;

		if (tried)
			return;
		tried = true;
		path = getenv("CLM_DEBUG_LOG");
		if (path == NULL)
			return; /* logging disabled unless explicitly enabled */
		fd =
		    open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
		if (fd < 0)
			return;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	hn = snprintf(head, sizeof(head), "%02d:%02d:%02d.%03ld [%d] ",
	    tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
	    (int)getpid());
	if (hn < 0 || (size_t)hn >= sizeof(head))
		return;

	/* Build the whole line on the heap and emit it in one write: no stdio
	 * buffer to hide errors, and a single append keeps concurrent writers
	 * from interleaving. */
	va_start(ap, fmt);
	va_copy(ap2, ap);
	mn = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (mn < 0) {
		va_end(ap2);
		return;
	}
	total = (size_t)hn + (size_t)mn + 1; /* + newline */
	line = malloc(total + 1);            /* + NUL for vsnprintf */
	if (line == NULL) {
		va_end(ap2);
		return;
	}
	memcpy(line, head, (size_t)hn);
	(void)vsnprintf(line + hn, (size_t)mn + 1, fmt, ap2);
	va_end(ap2);
	line[total - 1] = '\n';

	for (off = 0; off < total;) {
		ssize_t w = write(fd, line + off, total - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}
	free(line);
}
