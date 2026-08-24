// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stddef.h>

#include "tap.h"

static int
pass(void)
{
	TAP_CHECK(1, "a true expression passes");
	return 0;
}

static int
rejects_bad_registration(void)
{
	TAP_CHECK(tap_add(NULL, pass) == -EINVAL, "NULL test name is rejected");
	TAP_CHECK(tap_add("", pass) == -EINVAL, "empty test name is rejected");
	TAP_CHECK(tap_add("null callback", NULL) == -EINVAL,
	    "NULL callback is rejected");
	return 0;
}

int
main(void)
{
	TAP_CHECK(tap_add("pass", pass) == 0, "register pass test");
	TAP_CHECK(tap_add("reject bad registration", rejects_bad_registration) ==
	        0,
	    "register validation test");
	return tap_run();
}
