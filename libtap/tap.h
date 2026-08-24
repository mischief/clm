// SPDX-License-Identifier: ISC
#ifndef CLM_TAP_H
#define CLM_TAP_H

#include <stdbool.h>

/* A deliberately small, process-local TAP 13 runner for C tests. Each
 * registered test returns 0 on success and non-zero on failure. */
typedef int (*tap_fn)(void);

/* Register a named test. The harness copies name. Returns 0 or -errno. */
int tap_add(const char *name, tap_fn fn);

/* Run registered tests, writing TAP 13 to stdout. Returns 0 only if every
 * test passes. */
int tap_run(void);

/* Write a TAP diagnostic line for the test currently running. */
void tap_diag(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Release all registered tests. Useful for tests of the harness itself. */
void tap_reset(void);

/* Record a failed expectation for the current test and write a TAP diagnostic.
 * Tests may continue after this call to report more than one failed check. */
void tap_check(bool passed, const char *file, int line, const char *context,
    const char *expression);

/* Check an expression with a readable context string. The enclosing test must
 * return 0 on normal completion; tap_run() records this test as failed if any
 * TAP_CHECK within it failed. */
#define TAP_CHECK(expr, context)                                              \
	tap_check(!!(expr), __FILE__, __LINE__, (context), #expr)

#endif /* CLM_TAP_H */
