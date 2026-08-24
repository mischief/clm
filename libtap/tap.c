// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tap.h"

struct tap_test {
	char *name;
	tap_fn fn;
	void *arg;
};

static struct tap_test *tests;
static size_t ntests;
static size_t cap;
static bool running;
static bool current_failed;

int
tap_add(const char *name, tap_fn fn, void *arg)
{
	struct tap_test *p;
	char *copy;

	if (name == NULL || name[0] == '\0' || fn == NULL)
		return -EINVAL;
	if (running)
		return -EBUSY;
	if (ntests == cap) {
		size_t newcap = cap ? cap * 2 : 16;

		if (newcap < cap || newcap > SIZE_MAX / sizeof(*tests))
			return -ENOMEM;
		p = realloc(tests, newcap * sizeof(*tests));
		if (p == NULL)
			return -ENOMEM;
		tests = p;
		cap = newcap;
	}
	copy = strdup(name);
	if (copy == NULL)
		return -ENOMEM;
	tests[ntests].name = copy;
	tests[ntests].fn = fn;
	tests[ntests].arg = arg;
	ntests++;
	return 0;
}

void
tap_add_or_die(const char *name, tap_fn fn, void *arg, const char *file,
    int line)
{
	int r = tap_add(name, fn, arg);

	if (r == 0)
		return;
	fprintf(stderr, "%s:%d: TAP_ADD(%s) failed: %s\n", file, line,
	    name != NULL ? name : "(null)", strerror(-r));
	exit(EXIT_FAILURE);
}

void
tap_diag(const char *fmt, ...)
{
	va_list ap;

	fputs("# ", stdout);
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

void
tap_check(bool passed, const char *file, int line, const char *context,
    const char *expression)
{
	if (!passed) {
		current_failed = true;
		tap_diag("%s:%d: %s: %s", file, line, context, expression);
	}
}

int
tap_run(void)
{
	size_t i;
	int failed = 0;

	running = true;
	puts("TAP version 13");
	printf("1..%zu\n", ntests);
	for (i = 0; i < ntests; i++) {
		int r;

		current_failed = false;
		r = tests[i].fn(tests[i].arg);
		if (r == 0 && !current_failed)
			printf("ok %zu - %s\n", i + 1, tests[i].name);
		else {
			printf("not ok %zu - %s\n", i + 1, tests[i].name);
			failed = 1;
		}
	}
	running = false;
	return failed;
}

void
tap_reset(void)
{
	size_t i;

	if (running)
		return;
	for (i = 0; i < ntests; i++)
		free(tests[i].name);
	free(tests);
	tests = NULL;
	ntests = 0;
	cap = 0;
}
