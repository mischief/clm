// SPDX-License-Identifier: ISC
#include "clm/history.h"
#include "clm/cleanup.h"

#include <assert.h>
#include <errno.h>
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
 * Returns 0 on success (including the no-op case where there is nothing old
 * enough to compact), or a negative errno.
 */
int
clm_history_compact(struct clm_history *h, const char *summary, size_t keep_recent)
{
	struct clm_message *m, *cut = NULL, *sys_last = NULL, *summ;
	struct clm_message *first_nonsys;
	size_t users_seen = 0;

	if (h == NULL || summary == NULL)
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

	/*
	 * Walk backward from the tail, counting user messages. The cut is the
	 * keep_recent-th user message from the end -- the oldest turn we keep.
	 * Everything strictly before it (and after the system prologue) is
	 * summarized. Cutting at a user message keeps whole turns, so tool pairs
	 * stay intact.
	 */
	if (keep_recent == 0)
		return -EINVAL;
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

	/* Nothing old enough to compact: <= keep_recent user turns present. */
	if (cut == NULL || cut == first_nonsys)
		return 0;

	/* Build the summary message before mutating, so a failure is a no-op. */
	summ = clm_message_create(CLM_ROLE_USER);
	if (summ == NULL)
		return -ENOMEM;
	summ->content = strdup(summary);
	if (summ->content == NULL) {
		clm_message_free(summ);
		return -ENOMEM;
	}

	/* Remove [first_nonsys .. cut) and free them. */
	m = first_nonsys;
	while (m != NULL && m != cut) {
		struct clm_message *next = TAILQ_NEXT(m, entries);
		TAILQ_REMOVE(h, m, entries);
		clm_message_free(m);
		m = next;
	}

	/* Insert the summary where the old turns were: before the kept tail. */
	TAILQ_INSERT_BEFORE(cut, summ, entries);
	return 0;
}

struct clm_message *
clm_history_add_system(struct clm_history *h, const char *content)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_SYSTEM);
	if (m == NULL)
		return NULL;

	if (content) {
		m->content = strdup(content);
		if (m->content == NULL) {
			free(m);
			return NULL;
		}
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_user(struct clm_history *h, const char *content)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_USER);
	if (m == NULL)
		return NULL;

	if (content) {
		m->content = strdup(content);
		if (m->content == NULL) {
			free(m);
			return NULL;
		}
	}

	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_assistant_text(struct clm_history *h, const char *content)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_ASSISTANT);
	if (m == NULL)
		return NULL;

	if (content) {
		m->content = strdup(content);
		if (m->content == NULL) {
			free(m);
			return NULL;
		}
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

	m->content = NULL;
	TAILQ_INSERT_TAIL(h, m, entries);
	return m;
}

struct clm_message *
clm_history_add_tool_result(struct clm_history *h, const char *tool_call_id,
    const char *tool_name, const char *content)
{
	struct clm_message *m = clm_message_create(CLM_ROLE_TOOL);
	if (m == NULL)
		return NULL;

	if (tool_call_id) {
		m->tool_call_id = strdup(tool_call_id);
		if (m->tool_call_id == NULL) {
			free(m);
			return NULL;
		}
	}

	if (tool_name) {
		m->tool_name = strdup(tool_name);
		if (m->tool_name == NULL) {
			free(m->tool_call_id);
			free(m);
			return NULL;
		}
	}

	if (content) {
		m->content = strdup(content);
		if (m->content == NULL) {
			free(m->tool_name);
			free(m->tool_call_id);
			free(m);
			return NULL;
		}
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
		if (m->content != NULL && strcmp(m->content, stub) == 0)
			continue;
		char *dup = strdup(stub);
		if (dup == NULL)
			return -ENOMEM;
		free(m->content);
		m->content = dup;
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

static struct json_object *
clm_tool_call_to_json(const struct clm_tool_call *tc)
{
	struct json_object *tool_call = json_object_new_object();
	if (tool_call == NULL)
		return NULL;

	if (tc->id) {
		json_object_object_add(tool_call, "id", json_object_new_string(tc->id));
	}
	json_object_object_add(tool_call, "type", json_object_new_string("function"));

	struct json_object *func = json_object_new_object();
	if (func == NULL) {
		json_object_put(tool_call);
		return NULL;
	}

	if (tc->name) {
		json_object_object_add(func, "name", json_object_new_string(tc->name));
	}
	if (tc->args) {
		json_object_object_add(func, "arguments", json_object_new_string(tc->args));
	}
	json_object_object_add(tool_call, "function", func);

	return tool_call;
}

static struct json_object *
clm_message_to_json(const struct clm_message *m)
{
	json_cleanup struct json_object *msg = NULL;
	msg = json_object_new_object();
	ASSERT_RETURN(msg != NULL, NULL);

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

	struct json_object *jrole = json_object_new_string(role_str);
	ASSERT_RETURN(jrole != NULL, NULL);
	json_object_object_add(msg, "role", jrole);

	if (m->content) {
		struct json_object *jcontent = json_object_new_string(m->content);
		ASSERT_RETURN(jcontent != NULL, NULL);
		json_object_object_add(msg, "content", jcontent);
	} else if (m->role == CLM_ROLE_ASSISTANT) {
		json_object_object_add(msg, "content", json_object_new_null());
	}

	if (m->role == CLM_ROLE_TOOL) {
		if (m->tool_call_id) {
			struct json_object *jtid = json_object_new_string(m->tool_call_id);
			ASSERT_RETURN(jtid != NULL, NULL);
			json_object_object_add(msg, "tool_call_id", jtid);
		}
	}

	if (m->role == CLM_ROLE_ASSISTANT && !TAILQ_EMPTY(&m->tool_calls)) {
		struct json_object *tool_calls_arr = json_object_new_array();
		ASSERT_RETURN(tool_calls_arr != NULL, NULL);

		struct clm_tool_call *tc;
		TAILQ_FOREACH(tc, &m->tool_calls, entries) {
			struct json_object *tc_json = clm_tool_call_to_json(tc);
			ASSERT_RETURN(tc_json != NULL, NULL);
			json_object_array_add(tool_calls_arr, tc_json);
		}
		json_object_object_add(msg, "tool_calls", tool_calls_arr);
	}

	struct json_object *ret = msg;
	msg = NULL;
	return ret;
}

struct json_object *
clm_history_to_json(const struct clm_history *h)
{
	struct json_object *messages_arr = json_object_new_array();
	if (messages_arr == NULL)
		return NULL;

	struct clm_message *m;
	TAILQ_FOREACH(m, h, entries) {
		struct json_object *msg_json = clm_message_to_json(m);
		if (msg_json == NULL) {
			json_object_put(messages_arr);
			return NULL;
		}
		json_object_array_add(messages_arr, msg_json);
	}

	return messages_arr;
}
