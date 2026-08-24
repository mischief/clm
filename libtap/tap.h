// SPDX-License-Identifier: ISC
#ifndef CLM_TAP_H
#define CLM_TAP_H

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

/* Fail the current test with both a readable context string and the failed
 * expression. Tests return int, so an assertion can simply return failure. */
#define TAP_CHECK(expr, context)                                              \
	do {                                                                   \
		if (!(expr)) {                                                 \
			tap_diag("%s:%d: %s: %s", __FILE__, __LINE__,          \
			    (context), #expr);                                      \
			return 1;                                                  \
		}                                                              \
	} while (0)

#endif /* CLM_TAP_H */
