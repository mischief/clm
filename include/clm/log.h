// SPDX-License-Identifier: ISC
#ifndef CLM_LOG_H
#define CLM_LOG_H

#include "clm/clm_export.h"

/*
 * Internal debug logging. Writes one line per call to the file named by
 * $CLM_DEBUG_LOG. Disabled (a no-op) when that variable is unset, so nothing
 * is logged unless explicitly enabled. Lazily opened on first use; also a
 * no-op if the file cannot be opened. Not part of the versioned public
 * contract and this header isn't installed -- but it IS exported from
 * libclm (CLM_API, covered by libclm.map's clm_* wildcard) so libclmuv/
 * libclmlua/frontends can call the one copy in libclm instead of each
 * needing its own private statically-linked copy. Yes, that makes it a
 * real (if undocumented) symbol in libclm.so's dynamic table -- acceptable
 * since it's just a debug logger, not something a caller could misuse.
 */
CLM_API void clm_debug(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* CLM_LOG_H */
