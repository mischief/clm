// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stddef.h>

#include "tap.h"

static int
pass(void *arg)
{
	int *seen = arg;

	TAP_CHECK(seen != NULL, "test context is supplied");
	(*seen)++;
	TAP_CHECK(1, "a true expression passes");
	return 0;
}

static int
verify_context(void *arg)
{
	int *seen = arg;

	TAP_CHECK(*seen == 1, "runner passes registered context");
	return 0;
}

static int
rejects_bad_registration(void *arg)
{
	(void)arg;
	TAP_CHECK(
	    tap_add(NULL, pass, NULL) == -EINVAL, "NULL test name is rejected");
	TAP_CHECK(
	    tap_add("", pass, NULL) == -EINVAL, "empty test name is rejected");
	TAP_CHECK(tap_add("null callback", NULL, NULL) == -EINVAL,
	    "NULL callback is rejected");
	return 0;
}

int
main(void)
{
	int seen = 0;

	TAP_ADD("pass", pass, &seen);
	TAP_ADD("reject bad registration", rejects_bad_registration, NULL);
	TAP_ADD("verify context", verify_context, &seen);
	return tap_run();
}
