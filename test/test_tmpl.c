// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tmpl/tmpl.h"
#include "include/useful.h"
#include "banned.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			        __LINE__);                                     \
			failures++;                                            \
		}                                                              \
	} while (0)

static void
test_new(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new returns 0");
	CHECK(t != NULL, "tmpl_new sets pointer");
}

static void
test_refcount(void)
{
	_cleanup_tmpl_ struct tmpl *a = NULL;
	struct tmpl *b;
	int r;

	r = tmpl_new(&a);
	CHECK(r == 0, "tmpl_new");

	b = tmpl_ref(a);
	CHECK(b == a, "tmpl_ref returns same pointer");

	/* drop one ref; object must still be alive */
	tmpl_unref(b);
	CHECK(a != NULL, "object alive after one unref");
	/* a is released by _cleanup_tmpl_ */
}

static void
test_unrefp_nulls(void)
{
	struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");
	tmpl_unrefp(&t);
	CHECK(t == NULL, "tmpl_unrefp nulls pointer");
}

static void
test_parse(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");

	r = tmpl_parse(t, "foo=bar", 7);
	CHECK(r == 0, "tmpl_parse succeeds");
	CHECK(strcmp(tmpl_key(t), "foo") == 0, "key is foo");
	CHECK(strcmp(tmpl_val(t), "bar") == 0, "val is bar");
}

static void
test_parse_empty_val(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");

	r = tmpl_parse(t, "foo=", 4);
	CHECK(r == 0, "tmpl_parse empty val");
	CHECK(strcmp(tmpl_key(t), "foo") == 0, "key is foo");
	CHECK(strcmp(tmpl_val(t), "") == 0, "val is empty");
}

static void
test_parse_no_equals(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");

	r = tmpl_parse(t, "foobar", 6);
	CHECK(r == -ENOMSG, "tmpl_parse returns -ENOMSG for no '='");
}

static void
test_parse_invalid(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");

	r = tmpl_parse(t, NULL, 0);
	CHECK(r == -EINVAL, "tmpl_parse NULL input returns -EINVAL");

	r = tmpl_parse(t, "foo=bar", 0);
	CHECK(r == -EINVAL, "tmpl_parse zero len returns -EINVAL");

	r = tmpl_parse(NULL, "foo=bar", 7);
	CHECK(r == -EINVAL, "tmpl_parse NULL object returns -EINVAL");
}

static void
test_parse_overwrites(void)
{
	_cleanup_tmpl_ struct tmpl *t = NULL;
	int r;

	r = tmpl_new(&t);
	CHECK(r == 0, "tmpl_new");

	r = tmpl_parse(t, "a=1", 3);
	CHECK(r == 0, "first parse");

	r = tmpl_parse(t, "b=2", 3);
	CHECK(r == 0, "second parse");
	CHECK(strcmp(tmpl_key(t), "b") == 0, "key updated to b");
	CHECK(strcmp(tmpl_val(t), "2") == 0, "val updated to 2");
}

int
main(void)
{
	test_new();
	test_refcount();
	test_unrefp_nulls();
	test_parse();
	test_parse_empty_val();
	test_parse_no_equals();
	test_parse_invalid();
	test_parse_overwrites();

	if (failures > 0) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
