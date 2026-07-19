// SPDX-License-Identifier: ISC
#ifndef CLM_SESSION_INTERNAL_H
#define CLM_SESSION_INTERNAL_H

#include <stddef.h>

#include <cjson/cJSON.h>

#include "clm/history.h"

/*
 * Internal line parser, exposed for the fuzz harness (which compiles
 * session.c directly): parse one JSONL line into hist, stealing a meta
 * object into *out_meta (may be NULL). Returns 0 (bad lines are
 * skipped), -ENOMEM, or -EPROTONOSUPPORT for a too-new meta version.
 */
int session_parse_line(struct clm_history *hist, const char *line, size_t len,
                       cJSON **out_meta);

#endif /* CLM_SESSION_INTERNAL_H */
