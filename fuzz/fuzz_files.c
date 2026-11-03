// SPDX-License-Identifier: ISC
/*
 * Fuzz target for the files endpoint helpers. The input is an upload reply
 * body for clm_files_parse_id, and the base64 half of an image data URL
 * for clm_files_multipart.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afl.h"
#include "clm/internal.h"

static const char prefix[] = "data:image/png;base64,";

static void
check_id(const char *s)
{
	char *id = clm_files_parse_id(s);

	if (id == NULL)
		return;
	assert(id[0] != '\0' && strlen(id) < 256);
	assert(strspn(id,
	           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	           "0123456789_-") == strlen(id));
	free(id);
}

static void
check_multipart(const char *s, size_t size)
{
	char boundary[CLM_FILE_BOUNDARY_LEN];
	char tail[CLM_FILE_BOUNDARY_LEN + 16];
	size_t len = 0, tl;
	char *url = malloc(sizeof(prefix) + size);
	char *b;

	if (url == NULL)
		return;
	memcpy(url, prefix, sizeof(prefix) - 1);
	memcpy(url + sizeof(prefix) - 1, s, size + 1);
	b = clm_files_multipart(url, boundary, &len);
	free(url);
	if (b == NULL)
		return;
	tl = (size_t)snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
	assert(len > tl && b[len] == '\0');
	assert(strncmp(b, "--", 2) == 0 &&
	    strncmp(b + 2, boundary, strlen(boundary)) == 0);
	assert(memcmp(b + len - tl, tail, tl) == 0);
	free(b);
}

static void
fuzz_one(const uint8_t *data, size_t size)
{
	/* A NUL-terminated heap copy, so ASan sees any read past the end. */
	char *s = malloc(size + 1);

	if (s == NULL)
		return;
	memcpy(s, data, size);
	s[size] = '\0';
	check_id(s);
	check_multipart(s, strlen(s));
	free(s);
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
