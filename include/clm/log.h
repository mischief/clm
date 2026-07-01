// SPDX-License-Identifier: ISC
#ifndef CLM_LOG_H
#define CLM_LOG_H

/*
 * Internal debug logging. Writes one line per call to the file named by
 * $CLM_DEBUG_LOG. Disabled (a no-op) when that variable is unset, so nothing
 * is logged unless explicitly enabled. Lazily opened on first use; also a
 * no-op if the file cannot be opened. Library-internal only -- not part of
 * the public contract and not installed.
 */
void clm_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* CLM_LOG_H */
