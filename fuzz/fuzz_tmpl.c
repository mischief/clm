// SPDX-License-Identifier: ISC
// NOLINTBEGIN(bugprone-reserved-identifier) -- AFL++ __AFL_* macros
#include <stddef.h>
#include <unistd.h>

#include "tmpl/tmpl.h"
#include "include/useful.h"
#include "banned.h"

#ifndef __AFL_FUZZ_TESTCASE_LEN
ssize_t fuzz_len;
#define __AFL_FUZZ_TESTCASE_LEN fuzz_len
unsigned char fuzz_buf[1024000];
#define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
#define __AFL_FUZZ_INIT() void sync(void);
#define __AFL_LOOP(x)                                                          \
	((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0 ? 1 : 0)
#define __AFL_INIT() sync()
#endif
// NOLINTEND(bugprone-reserved-identifier)

__AFL_FUZZ_INIT()

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif

	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(10000)) {
		_cleanup_tmpl_ struct tmpl *t = NULL;
		int len = __AFL_FUZZ_TESTCASE_LEN;

		if (tmpl_new(&t) < 0)
			continue;
		(void)tmpl_parse(t, (const char *)buf, (size_t)len);
	}

	return 0;
}
