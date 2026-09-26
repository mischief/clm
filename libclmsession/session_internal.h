// SPDX-License-Identifier: ISC
#ifndef CLM_SESSION_INTERNAL_H
#define CLM_SESSION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "clm/history.h"

/*
 * Internal line parser, exposed for the fuzz harness (which compiles
 * session.c directly): parse one JSONL line into hist, stealing a meta
 * object into *out_meta (may be NULL). Images a message names are read
 * from blob_dir (may be NULL: they become notes). Returns 0 (bad lines are
 * skipped), -ENOMEM, or -EPROTONOSUPPORT for a too-new meta version.
 */
int session_parse_line(struct clm_history *hist, const char *line, size_t len,
    cJSON **out_meta, const char *blob_dir);

/* SHA-256 of data as 64 lowercase hex digits and a NUL. */
void session_sha256_hex(const uint8_t *data, size_t len, char out[65]);

#endif /* CLM_SESSION_INTERNAL_H */
