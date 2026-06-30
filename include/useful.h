/* SPDX-License-Identifier: ISC */
#ifndef CLM_USEFUL_H
#define CLM_USEFUL_H

/*
 * ASSERT_RETURN: enforce a precondition at a public API boundary.
 *
 * Unlike assert(3), this is always active and returns r rather than aborting.
 * Use it at the top of public functions to validate caller-supplied arguments.
 * Use assert(3) for internal invariants that should never fail.
 */
#define ASSERT_RETURN(expr, r)                                                 \
	do {                                                                   \
		if (!(expr))                                                   \
			return (r);                                            \
	} while (0)

#endif /* CLM_USEFUL_H */
