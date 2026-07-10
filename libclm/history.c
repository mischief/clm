// SPDX-License-Identifier: ISC
#include "clm/history.h"
#include "clm/cleanup.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "useful.h"

void
clm_tool_result_free(struct clm_tool_result *r)
{
	if (r == NULL)
		return;
	free(r->tool_call_id);
	free(r->content);
	free(r);
}

void
clm_history_init(struct clm_history *h)
{
	TAILQ_INIT(h);
}

static void
clm_tool_call_free(struct clm_tool_call *tc)
{
	if (tc) {
		free(tc->id);
		free(tc->name);
		free(tc->args);
		free(tc);
	}
}

static void
clm_message_free(struct clm_message *m)
{
	if (m) {
		free(m->content);
		free(m->tool_call_id);
		free(m->tool_name);
		struct clm_tool_call *tc, *tc_next;
		for (tc = TAILQ_FIRST(&m->tool_calls); tc != NULL; tc = tc_next) {
			tc_next = TAILQ_NEXT(tc, entries);
			TAILQ_REMOVE(&m->tool_calls, tc, entries);
			clm_tool_call_free(tc);
		}
		free(m);
	}
}

void
clm_history_free(struct clm_history *h)
{
	struct clm_message *m, *m_next;
	for (m = TAILQ_FIRST(h); m != NULL; m = m_next) {
		m_next = TAILQ_NEXT(m, entries);
		TAILQ_REMOVE(h, m, entries);
		clm_message_free(m);
	}
}

static struct clm_message *
clm_message_create(enum clm_role role)
{
	struct clm_message *m = calloc(1, sizeof(struct clm_message));
	if (m == NULL)
		return NULL;

	m->role = role;
	TAILQ_INIT(&m->tool_calls);
	return m;
}

/*
 * Duplicate content into m->content and set m->content_len to its length,
 * capped at UINT16_MAX (a single message cannot exceed 64 KiB - 1; content
 * longer than that is truncated to fit). Every insertion path routes through
 * here so content_len is never stale or left unset. A NULL content is a
 * no-op: m->content stays NULL, m->content_len stays 0.
 *
 * If cz is non-NULL and content is at least cz->min_len bytes, content is
 * compressed via cz->write before storing; m->content_compressed is set and
 * m->content_len becomes the compressed length. Falls back to plain storage
 * (content_compressed = false) if cz is NULL, content is too short,
 * compression fails, or the compressed result is not actually smaller.
 */
static int
message_set_content(struct clm_message *m, const char *content,
    const struct clm_compressor *cz)
{
	size_t len;

	if (content == NULL) {
		free(m->content);
		m->content = NULL;
		m->content_len = 0;
		m->content_compressed = false;
		return 0;
	}

	len = strlen(content);
	if (len > UINT16_MAX)
		len = UINT16_MAX;

	if (cz != NULL && cz->write != NULL && len >= cz->min_len) {
		char *packed = NULL;
		size_t packed_len = 0;
		if (cz->write(cz->ctx, content, len, &packed, &packed_len) == 0 &&
		    packed_len < len && packed_len <= UINT16_MAX) {
			free(m->content);
			m->content = packed;
			m->content_len = (uint16_t)packed_len;
			m->content_compressed = true;
			return 0;
		}
		free(packed); /* NULL-safe: discard a failed/unhelpful attempt */
	}

	char *copy = malloc(len + 1);
	if (copy == NULL)
		return -ENOMEM;
	memcpy(copy, content, len);
	copy[len] = '\0';

	free(m->content);
	m->content = copy;
	m->content_len = (uint16_t)len;
	m->content_compressed = false;
	return 0;
}

/*
 * Replace old conversation turns with a single summary message, keeping the
 * leading system message(s) and the most recent whole user turns verbatim.
 *
 * The cut always lands on a user-message boundary walked back from the tail:
 * a tool exchange (assistant tool_calls -> tool results) lives entirely within
 * one user turn, so cutting only at user messages never orphans a tool result
 * from its call. keep_recent is the number of trailing user turns to preserve;
 * the summary is injected as a framed user message right after the system
 * prologue (the chat template forbids a non-leading system message).
 *
 * Fallback for agentic histories: a headless/forever-mode run has ONE user
 * message ever (the initial mission prompt) followed by an arbitrarily long
 * autonomous chain of tool exchanges inside that single turn. The user-boundary
 * walk above then finds no cut, and compaction is a permanent no-op -- which is
 * how a live agent got stuck re-summarizing its entire history after every tool
 * round-trip, discarding the summary each time (the "compact forever" loop:
 * over threshold -> summarize full history -> fold nothing -> still over
 * threshold). So when no user cut exists, we cut at tool-exchange boundaries
 * instead: keep the last keep_recent assistant-with-tool_calls messages (each
 * heads one exchange; its results follow it), fold everything older. This is
 * equally split-safe -- a result can never precede its own call, so removing a
 * prefix that ends just before an exchange head never orphans a result. The
 * mission user message is treated as part of the prologue and kept verbatim:
 * it is the only statement of what the agent is supposed to be doing, and
 * trusting a lossy LLM summary to re-state the mission is how agents wander
 * off-task after compaction.
 *
 * Returns the number of messages folded into the summary (0 = nothing old
 * enough to compact; the history is unchanged and no summary was inserted),
 * or a negative errno. Callers should treat 0 as "no progress", not success:
 * re-triggering compaction on an unchanged history will no-op forever.
 */
int
clm_history_compact(struct clm_history *h, const char *summary, size_t keep_recent,
    const struct clm_compressor *cz)
{
	struct clm_message *m, *cut = NULL, *sys_last = NULL, *summ;
	struct clm_message *first_nonsys, *start;
	size_t users_seen = 0;
	int folded = 0;

	if (h == NULL || summary == NULL)
		return -EINVAL;
	if (keep_recent == 0)
		return -EINVAL;

	/* Find the end of the leading system prologue (kept as-is). */
	TAILQ_FOREACH(m, h, entries) {
		if (m->role != CLM_ROLE_SYSTEM)
			break;
		sys_last = m;
	}
	first_nonsys = m; /* first message we might summarize, or NULL */
	if (first_nonsys == NULL)
		return 0; /* nothing but system messages */
	start = first_nonsys; /* first message actually folded */

	/*
	 * Walk backward from the tail, counting user messages. The cut is the
	 * keep_recent-th user message from the end -- the oldest turn we keep.
	 * Everything strictly before it (and after the system prologue) is
	 * summarized. Cutting at a user message keeps whole turns, so tool pairs
	 * stay intact.
	 */
	for (m = TAILQ_LAST(h, clm_history); m != NULL;
	    m = TAILQ_PREV(m, clm_history, entries)) {
		if (m == sys_last)
			break;
		if (m->role == CLM_ROLE_USER) {
			users_seen++;
			if (users_seen == keep_recent) {
				cut = m;
				break;
			}
		}
	}

	/*
	 * No user-boundary cut: single-user-turn agentic history (see the
	 * function comment). Extend the prologue through the first user
	 * message (the mission), then cut at the keep_recent-th exchange head
	 * (assistant message carrying tool_calls) walked back from the tail.
	 */
	if (cut == NULL || cut == first_nonsys) {
		struct clm_message *mission = NULL;
		size_t heads_seen = 0;

		for (m = first_nonsys; m != NULL; m = TAILQ_NEXT(m, entries)) {
			if (m->role == CLM_ROLE_USER) {
				mission = m;
				break;
			}
		}
		start = mission != NULL ? TAILQ_NEXT(mission, entries)
		                        : first_nonsys;
		if (start == NULL)
			return 0; /* mission is the whole tail */

		cut = NULL;
		for (m = TAILQ_LAST(h, clm_history); m != NULL && m != mission;
		    m = TAILQ_PREV(m, clm_history, entries)) {
			if (m == sys_last)
				break;
			if (m->role == CLM_ROLE_ASSISTANT &&
			    !TAILQ_EMPTY(&m->tool_calls)) {
				heads_seen++;
				if (heads_seen == keep_recent) {
					cut = m;
					break;
				}
			}
		}
		if (cut == NULL || cut == start)
			return 0; /* <= keep_recent exchanges: nothing to fold */
	}

	/* Build the summary message before mutating, so a failure is a no-op. */
	summ = clm_message_create(CLM_ROLE_USER);
	if (summ == NULL)
		return -ENOMEM;
	if (message_set_content(summ, summary, cz) < 0) {
		clm_message_free(summ);
		return -ENOMEM;
	}

	/* Remove [start .. cut) and free them. */
	m = start;
	while (m != NULL && m != cut) {
		struct clm_message *next = TAILQ_NEXT(m, entries);
		TAILQ_REMOVE(h, m, entries);
		clm_message_free(m);
		folded++;
		m = next;
	}

	/* Insert the summary where the old turns were: before the kept tail. */
	TAILQ_INSERT_BEFORE(cut, summ, entries);
	return folded;
}

struct clm_message *
clm_history_add_system(struct clm_history *h, const char *content,
    const struct clm_compressor *cz)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_SYSTEM);
	if (m == NULL)
		return NULL;

	if (message_set_content(m, content, cz) < 0) {
		clm_message_free(m);
		return NULL;
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_user(struct clm_history *h, const char *content,
    const struct clm_compressor *cz)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_USER);
	if (m == NULL)
		return NULL;

	if (message_set_content(m, content, cz) < 0) {
		clm_message_free(m);
		return NULL;
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_assistant_text(struct clm_history *h, const char *content,
    const struct clm_compressor *cz)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_ASSISTANT);
	if (m == NULL)
		return NULL;

	if (message_set_content(m, content, cz) < 0) {
		clm_message_free(m);
		return NULL;
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_assistant_tool_calls(struct clm_history *h)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_ASSISTANT);
	if (m == NULL)
		return NULL;

	/* content/content_len already NULL/0 from calloc in clm_message_create. */
	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_tool_result(struct clm_history *h, const char *tool_call_id,
    const char *tool_name, const char *content, const struct clm_compressor *cz)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_TOOL);
	if (m == NULL)
		return NULL;

	if (tool_call_id) {
		m->tool_call_id = strdup(tool_call_id);
		if (m->tool_call_id == NULL) {
			clm_message_free(m);
			return NULL;
		}
	}

	if (tool_name) {
		m->tool_name = strdup(tool_name);
		if (m->tool_name == NULL) {
			clm_message_free(m);
			return NULL;
		}
	}

	if (message_set_content(m, content, cz) < 0) {
		clm_message_free(m);
		return NULL;
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

int
clm_history_supersede_tool(struct clm_history *h, const char *tool_name,
    const char *stub)
{
	struct clm_message *m, *batch_head = NULL;
	int stubbed = 0;

	if (h == NULL || tool_name == NULL || stub == NULL)
		return -EINVAL;

	/*
	 * Find the assistant message that opened the current batch: results
	 * recorded after it belong to in-flight sibling invocations and must
	 * survive (two calls to the same tool in one batch would otherwise
	 * stub each other's fresh output).
	 */
	for (m = TAILQ_LAST(h, clm_history); m != NULL;
	    m = TAILQ_PREV(m, clm_history, entries)) {
		if (m->role == CLM_ROLE_ASSISTANT && !TAILQ_EMPTY(&m->tool_calls)) {
			batch_head = m;
			break;
		}
	}

	TAILQ_FOREACH(m, h, entries) {
		if (m == batch_head)
			break;
		if (m->role != CLM_ROLE_TOOL || m->tool_name == NULL)
			continue;
		if (strcmp(m->tool_name, tool_name) != 0)
			continue;
		/* Already stubbed on an earlier pass: leave the bytes alone. */
		if (!m->content_compressed && m->content != NULL &&
		    strcmp(m->content, stub) == 0)
			continue;
		/* No compressor here: the stub is always short and plain, never
		 * worth compressing, and re-stubbing must not leave a stale
		 * content_compressed flag from the content it replaces. */
		if (message_set_content(m, stub, NULL) < 0)
			return -ENOMEM;
		stubbed++;
	}

	return stubbed;
}

struct clm_tool_call *
clm_message_add_tool_call(struct clm_message *m, const char *id, const char *name, const char *args)
{
	if (m == NULL || m->role != CLM_ROLE_ASSISTANT)
		return NULL;

	struct clm_tool_call *tc = calloc(1, sizeof(struct clm_tool_call));
	if (tc == NULL)
		return NULL;

	if (id) {
		tc->id = strdup(id);
		if (tc->id == NULL)
			goto fail;
	}

	if (name) {
		tc->name = strdup(name);
		if (tc->name == NULL)
			goto fail;
	}

	if (args) {
		tc->args = strdup(args);
		if (tc->args == NULL)
			goto fail;
	}

	TAILQ_INSERT_TAIL(&m->tool_calls, tc, entries);
	return tc;

fail:
	clm_tool_call_free(tc);
	return NULL;
}

static cJSON *
clm_tool_call_to_json(const struct clm_tool_call *tc)
{
	cJSON *tool_call = cJSON_CreateObject();
	if (tool_call == NULL)
		return NULL;

	if (tc->id) {
		cJSON_AddItemToObject(tool_call, "id", cJSON_CreateString(tc->id));
	}
	cJSON_AddItemToObject(tool_call, "type", cJSON_CreateString("function"));

	cJSON *func = cJSON_CreateObject();
	if (func == NULL) {
		cJSON_Delete(tool_call);
		return NULL;
	}

	if (tc->name) {
		cJSON_AddItemToObject(func, "name", cJSON_CreateString(tc->name));
	}
	if (tc->args) {
		cJSON_AddItemToObject(func, "arguments", cJSON_CreateString(tc->args));
	}
	cJSON_AddItemToObject(tool_call, "function", func);

	return tool_call;
}

static cJSON *
clm_message_to_json(const struct clm_message *m, const struct clm_compressor *cz)
{
	json_cleanup cJSON *msg = NULL;
	msg = cJSON_CreateObject();
	if (msg == NULL)
		return NULL;

	const char *role_str = NULL;
	switch (m->role) {
	case CLM_ROLE_SYSTEM:
		role_str = "system";
		break;
	case CLM_ROLE_USER:
		role_str = "user";
		break;
	case CLM_ROLE_ASSISTANT:
		role_str = "assistant";
		break;
	case CLM_ROLE_TOOL:
		role_str = "tool";
		break;
	}

	cJSON *jrole = cJSON_CreateString(role_str);
	if (jrole == NULL) {
		cJSON_Delete(msg);
		return NULL;
	}
	cJSON_AddItemToObject(msg, "role", jrole);

	if (m->content) {
		char *plain = m->content;
		bool free_plain = false;

		if (m->content_compressed) {
			/* Content was stored compressed but there is no decompressor
			 * to reverse it -- a caller/config mismatch, not recoverable
			 * here. */
			ASSERT_RETURN(cz != NULL && cz->read != NULL, NULL);
			if (cz->read(cz->ctx, m->content, m->content_len, &plain) < 0) {
				cJSON_Delete(msg);
				return NULL;
			}
			free_plain = true;
		}

		cJSON *jcontent = cJSON_CreateString(plain);
		if (free_plain)
			free(plain);
		if (jcontent == NULL) {
			cJSON_Delete(msg);
			return NULL;
		}
		cJSON_AddItemToObject(msg, "content", jcontent);
	} else if (m->role == CLM_ROLE_ASSISTANT) {
		cJSON_AddItemToObject(msg, "content", cJSON_CreateNull());
	}

	if (m->role == CLM_ROLE_TOOL) {
		if (m->tool_call_id) {
			cJSON *jtid = cJSON_CreateString(m->tool_call_id);
			if (jtid == NULL) {
				cJSON_Delete(msg);
				return NULL;
			}
			cJSON_AddItemToObject(msg, "tool_call_id", jtid);
		}
	}

	if (m->role == CLM_ROLE_ASSISTANT && !TAILQ_EMPTY(&m->tool_calls)) {
		cJSON *tool_calls_arr = cJSON_CreateArray();
		if (tool_calls_arr == NULL) {
			cJSON_Delete(msg);
			return NULL;
		}

		struct clm_tool_call *tc;
		TAILQ_FOREACH(tc, &m->tool_calls, entries) {
			cJSON *tc_json = clm_tool_call_to_json(tc);
			if (tc_json == NULL) {
				cJSON_Delete(tool_calls_arr);
				cJSON_Delete(msg);
				return NULL;
			}
			cJSON_AddItemToArray(tool_calls_arr, tc_json);
		}
		cJSON_AddItemToObject(msg, "tool_calls", tool_calls_arr);
	}

	cJSON *ret = msg;
	msg = NULL;
	return ret;
}

cJSON *
clm_history_to_json(const struct clm_history *h, const struct clm_compressor *cz)
{
	cJSON *messages_arr = cJSON_CreateArray();
	if (messages_arr == NULL)
		return NULL;

	struct clm_message *m;
	TAILQ_FOREACH(m, h, entries) {
		cJSON *msg_json = clm_message_to_json(m, cz);
		if (msg_json == NULL) {
			cJSON_Delete(messages_arr);
			return NULL;
		}
		cJSON_AddItemToArray(messages_arr, msg_json);
	}

	return messages_arr;
}
