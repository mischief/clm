// SPDX-License-Identifier: ISC
#ifndef CLM_HISTORY_H
#define CLM_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/queue.h>

#include <cjson/cJSON.h>

#include "clm/compress.h"

enum clm_role {
	CLM_ROLE_SYSTEM,
	CLM_ROLE_USER,
	CLM_ROLE_ASSISTANT,
	CLM_ROLE_TOOL,
};

/*
 * A single tool call requested by the assistant. id, name and args are owned
 * strings; args holds the raw JSON arguments object (e.g. "{\"path\": \"x\"}"),
 * parsed by the dispatcher when executing.
 */
struct clm_tool_call {
	char *id;
	char *name;
	char *args;
	TAILQ_ENTRY(clm_tool_call) entries;
};

TAILQ_HEAD(clm_tool_call_list, clm_tool_call);

/*
 * The result of executing a tool, linked to its originating call by
 * tool_call_id. Both fields are owned strings.
 */
struct clm_tool_result {
	char *tool_call_id;
	char *content;
};

void clm_tool_result_free(struct clm_tool_result *r);

/*
 * A single message in the conversation history. Fields used depend on role:
 *
 *   CLM_ROLE_SYSTEM    : content
 *   CLM_ROLE_USER      : content
 *   CLM_ROLE_ASSISTANT : content (may be NULL if tool_calls present)
 *                        tool_calls (empty unless the assistant requested calls)
 *   CLM_ROLE_TOOL      : content (the tool output), tool_call_id
 *
 * content and tool_call_id are owned strings or NULL. content_len is the
 * strlen() of content (0 if content is NULL), capped at UINT16_MAX -- a
 * single stored message cannot exceed 64 KiB - 1. This is tracked explicitly
 * (rather than relying on content always being NUL-terminated) so a
 * compressed representation of content has a length to work with without
 * re-deriving it, and so no caller has to assume content is a plain C string.
 *
 * tool_name (CLM_ROLE_TOOL only) records which tool produced the result.
 * It exists for clm_history_supersede_tool() -- the wire format links a
 * result to its call by tool_call_id alone, so serialization never emits
 * it.
 *
 * content_compressed is true when content holds opaque compressed bytes
 * rather than plain text (see clm/compress.h); always false unless an
 * embedder installed a compressor via clm_agent_set_compressor().
 */
struct clm_message {
	enum clm_role role;
	char *content;
	uint16_t content_len;
	bool content_compressed;
	char *tool_call_id;
	char *tool_name;
	struct clm_tool_call_list tool_calls;
	TAILQ_ENTRY(clm_message) entries;
};

TAILQ_HEAD(clm_history, clm_message);

/* Lifecycle. */
void clm_history_init(struct clm_history *h);
void clm_history_free(struct clm_history *h);

/*
 * Append messages. Each returns the new message on success or NULL on
 * allocation failure. The history takes ownership of duplicated content.
 *
 * cz, when non-NULL, is an optional compressor: content at least cz->min_len
 * bytes long is compressed via cz->write before storing, falling back to
 * plain storage if compression fails or does not shrink the content. Pass
 * NULL to always store content plain (the default for embedders that never
 * call clm_agent_set_compressor).
 */
struct clm_message *clm_history_add_system(struct clm_history *h,
    const char *content, const struct clm_compressor *cz);
struct clm_message *clm_history_add_user(struct clm_history *h,
    const char *content, const struct clm_compressor *cz);
struct clm_message *clm_history_add_assistant_text(struct clm_history *h,
    const char *content, const struct clm_compressor *cz);

/*
 * Append an assistant message that requests tool calls. Returns an empty
 * message whose tool_calls list is populated via clm_message_add_tool_call.
 */
struct clm_message *clm_history_add_assistant_tool_calls(struct clm_history *h);

/*
 * Append a tool result, linked to a prior call by tool_call_id. tool_name
 * (may be NULL) is kept for clm_history_supersede_tool() matching only. See
 * cz above.
 */
struct clm_message *clm_history_add_tool_result(struct clm_history *h,
    const char *tool_call_id, const char *tool_name, const char *content,
    const struct clm_compressor *cz);

/*
 * Replace the content of every tool result from tool_name that precedes
 * the current (latest) tool batch with the short stub string, e.g.
 * "[superseded by newer local_map]". Messages are stubbed in place --
 * never removed -- so each assistant tool_calls entry keeps its paired
 * result and the wire format stays valid. Results already stubbed are
 * left untouched, keeping earlier bytes stable for prefix caching; only
 * the most recent prior result actually changes on a typical call.
 * Returns the number of results stubbed, or a negative errno.
 */
int clm_history_supersede_tool(struct clm_history *h, const char *tool_name,
    const char *stub);

/* Attach a tool call to an assistant message. Returns the call or NULL. */
struct clm_tool_call *clm_message_add_tool_call(struct clm_message *m,
    const char *id, const char *name, const char *args);

/*
 * Replace old turns with a single summary message, keeping the leading system
 * prologue and the last keep_recent user turns verbatim. Cuts at user
 * boundaries so tool-call/result pairs are never split; when the history has
 * no user boundary to cut at (a single-user-turn agentic run), falls back to
 * cutting at tool-exchange boundaries, keeping the first user message (the
 * mission) verbatim -- see the implementation comment for the full rationale.
 * Returns the number of messages folded into the summary (0 = nothing folded,
 * history unchanged -- callers must treat this as "no progress", not success)
 * or a negative errno on allocation failure. See cz above.
 */
int clm_history_compact(struct clm_history *h, const char *summary,
    size_t keep_recent, const struct clm_compressor *cz);

/*
 * Serialize the entire history into a cJSON array suitable for the
 * "messages" field of a chat/completions request. Caller owns the returned
 * object (cJSON_Delete). Returns NULL on failure.
 *
 * cz, when non-NULL, must be the same compressor used to store any
 * compressed messages -- it decompresses content marked content_compressed
 * before serializing. Pass NULL if no compressor was ever installed.
 */
cJSON *clm_history_to_json(const struct clm_history *h,
    const struct clm_compressor *cz);

#endif /* CLM_HISTORY_H */
