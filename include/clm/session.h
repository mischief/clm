// SPDX-License-Identifier: ISC
#ifndef CLM_SESSION_H
#define CLM_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "clm/clm_export.h"
#include "clm/history.h"

/*
 * Session persistence: an append-only JSONL log of the conversation
 * history, one JSON object per line, so resuming a session replays the
 * transcript into a fresh agent (clm_agent_restore_history).
 *
 * File format (version 1), one file per session at <dir>/<id>.jsonl:
 *
 *   line 1:  {"type":"meta","v":1,"id":"...","created":<unix>,
 *             "model":"...","provider":"...","agent":"..."}
 *   line 2+: {"type":"msg", ...clm_message_to_json_full() shape...}
 *
 * Each message is appended as a single O_APPEND write, so a crash never
 * corrupts prior lines; the loader ignores a truncated or unparsable
 * final line and skips lines of unknown "type" (forward compatibility).
 *
 * The log is the raw transcript: history rewrites (compaction,
 * supersede stubs) are not represented, and a resumed session replays
 * the full uncompacted history (autocompaction re-fires as needed).
 *
 * All functions taking a dir accept NULL to mean the default state
 * directory (see clm_session_state_dir). All return 0 or a negative
 * errno unless noted.
 */

struct clm_session;

/*
 * Resolve the default session directory -- $XDG_STATE_HOME/clm, falling
 * back to $HOME/.local/state/clm -- into buf and create it (0700,
 * parents included) if missing.
 */
CLM_API int clm_session_state_dir(char *buf, size_t bufsz);

/*
 * Create a new session: generate an id, create <dir>/<id>.jsonl (0600)
 * and write the meta line. model/provider_name/agent_name may each be
 * NULL; they are recorded for listings only.
 */
CLM_API int clm_session_create(const char *dir, const char *model,
                               const char *provider_name,
                               const char *agent_name,
                               struct clm_session **out);

/*
 * Open an existing session for appending. -ENOENT if no such session,
 * -EINVAL if id is not a valid session id (ids are [0-9A-Za-z-] only;
 * anything else is rejected before touching the filesystem).
 */
CLM_API int clm_session_open(const char *dir, const char *id,
                             struct clm_session **out);

/*
 * Append one message as a single JSONL line. cz must be the compressor
 * the message was stored under (NULL if none) -- content is always
 * written plain. Intended to be called from an on_message callback.
 */
CLM_API int clm_session_append(struct clm_session *s,
                               const struct clm_message *m,
                               const struct clm_compressor *cz);

/* The session's id (borrowed, valid until clm_session_free). */
CLM_API const char *clm_session_id(const struct clm_session *s);

/*
 * True until a user or assistant message has been appended, this run or
 * (for a reopened session) a prior one. An empty session is not worth
 * resuming; discard it on exit instead of printing its id.
 */
CLM_API bool clm_session_is_empty(const struct clm_session *s);

/* Close the session and delete its file. Frees s. */
CLM_API int clm_session_discard(struct clm_session *s);

/* Close the session, keeping its file. */
CLM_API void clm_session_free(struct clm_session *s);

/*
 * Load a session's messages into hist (caller must have initialized it,
 * and owns it -- on failure it may hold a partial load; free it either
 * way). out_meta, when non-NULL, receives the parsed meta object or
 * NULL if the file has none (caller cJSON_Deletes). Returns -ENOENT if
 * no such session, -EPROTONOSUPPORT if the meta version is newer than
 * this library understands.
 */
CLM_API int clm_session_load(const char *dir, const char *id,
                             struct clm_history *hist, cJSON **out_meta);

/* One row of a session listing. All strings owned; may be NULL. */
struct clm_session_info {
	char *id;
	char *model;
	char *agent;
	int64_t created;
	char *first_user; /* snippet of the first real user message */
};

/*
 * List sessions in dir, newest first. *out is a malloc'd array of
 * *out_n entries (free via clm_session_list_free); 0 entries yields
 * *out == NULL. A missing dir is an empty listing, not an error.
 */
CLM_API int clm_session_list(const char *dir, struct clm_session_info **out,
                             size_t *out_n);
CLM_API void clm_session_list_free(struct clm_session_info *infos, size_t n);

#endif /* CLM_SESSION_H */
