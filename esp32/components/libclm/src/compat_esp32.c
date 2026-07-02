// SPDX-License-Identifier: ISC
/* Public getline/getdelim wrappers over newlib's __getdelim (see clm_compat.h). */
#include <stdio.h>
#include <sys/types.h>

extern ssize_t __getdelim(char **lineptr, size_t *n, int delim, FILE *stream);

ssize_t
getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
	return __getdelim(lineptr, n, delim, stream);
}

ssize_t
getline(char **lineptr, size_t *n, FILE *stream)
{
	return __getdelim(lineptr, n, '\n', stream);
}
