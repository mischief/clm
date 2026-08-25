// SPDX-License-Identifier: ISC
#ifndef CLM_FUZZ_AFL_H
#define CLM_FUZZ_AFL_H
/*
 * AFL++ persistent-mode boilerplate shared by the fuzz targets. Built with
 * a compiler that is not afl-clang-fast, the fallback below reads one case
 * from stdin instead, so a target still reproduces a crash on its own.
 */
#include <stddef.h>
#include <unistd.h>

// NOLINTBEGIN(bugprone-reserved-identifier) -- AFL++ __AFL_* macros
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

/* Cases per process before AFL++ forks a fresh one. */
#define CLM_FUZZ_LOOPS 10000

#endif /* CLM_FUZZ_AFL_H */
