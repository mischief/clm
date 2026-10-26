/* SPDX-License-Identifier: ISC */
#ifndef CLM_FILEIO_H
#define CLM_FILEIO_H

/*
 * Raw file and fd I/O, no stdio (see banned.h). Reads slurp the whole file;
 * callers split lines in memory. Writes report ENOSPC/quota at the write or
 * close, which stdio would have hidden behind its buffer.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h> /* vsnprintf (allowed); FILE I/O is poisoned by banned.h */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Write all len bytes to fd. Returns 0, or -errno on a write failure. */
static inline int
clm_write_all(int fd, const void *data, size_t len)
{
	const char *p = data;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

/* Write a NUL-terminated string to fd. Returns 0 or -errno. */
static inline int
clm_write_str(int fd, const char *s)
{
	return clm_write_all(fd, s, strlen(s));
}

/* Formatted write to fd, no stdio stream. Best-effort (terminal output). */
static inline void
clm_dprintf(int fd, const char *fmt, ...)
{
	va_list ap, ap2;
	char *buf;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		va_end(ap2);
		return;
	}
	(void)vsnprintf(buf, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	(void)clm_write_all(fd, buf, (size_t)n);
	free(buf);
}

/*
 * Read the whole file into a malloc'd, NUL-terminated buffer. On success
 * stores the buffer in *out (free it), its byte length in *lenp (may be
 * NULL), and returns 0. On failure returns -errno and leaves *out NULL.
 */
static inline int
clm_read_file(const char *path, char **out, size_t *lenp)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	int fd, saved;

	*out = NULL;
	if (lenp != NULL)
		*lenp = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	for (;;) {
		ssize_t n;
		if (len + 1 >= cap) {
			size_t ncap = cap != 0 ? cap * 2 : 8192;
			char *p = realloc(buf, ncap);
			if (p == NULL) {
				saved = ENOMEM;
				goto fail;
			}
			buf = p;
			cap = ncap;
		}
		n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			saved = errno;
			goto fail;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	(void)close(fd);
	if (buf == NULL) {
		buf = malloc(1);
		if (buf == NULL)
			return -ENOMEM;
	}
	buf[len] = '\0';
	*out = buf;
	if (lenp != NULL)
		*lenp = len;
	return 0;
fail:
	(void)close(fd);
	free(buf);
	return -saved;
}

/*
 * Write len bytes to path (created/truncated, mode 0666 & umask). Returns 0,
 * or -errno if the open, a write, or the final close fails.
 */
static inline int
clm_write_file(const char *path, const void *data, size_t len)
{
	int fd, rc;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0)
		return -errno;
	rc = clm_write_all(fd, data, len);
	if (rc != 0) {
		(void)close(fd);
		return rc;
	}
	if (close(fd) != 0)
		return -errno;
	return 0;
}

#endif /* CLM_FILEIO_H */
