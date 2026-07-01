// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>

#include "clm/agent.h"
#include "clm/http.h"
#include "clm/http_async.h"
#include "clm/llm.h"
#include "clm/tools.h"
#include "clm/history.h"
#include "clm/internal.h"
#include "clm/cleanup.h"
#include "clm/log.h"
#include "useful.h"
#include "banned.h"

static const char *default_system_prompt =
    "You are a helpful assistant. Answer from your own knowledge whenever you can. "
    "Use the provided tools only when the task requires reading or modifying the "
    "user's files or running a command on their system. When you already have the "
    "answer, reply directly without using tools.";

/* Minimum spacing between injected "current time" context updates. */
#define CLM_TIME_STAMP_INTERVAL 600 /* seconds (10 minutes) */

/*
 * Explains the automatic time-context injections to the model. Appended to the
 * system prompt once, so a mid-conversation "[context update]" line is treated
 * as silent ambient background rather than something the user said. The Qwen3
 * chat template rejects any system message that is not first, so per-turn
 * updates ride in a clearly framed user-role message instead (see
 * clm_agent_submit); this convention line is what keeps that transparent.
 */
static const char *time_context_note =
    "\n\nYou may periodically receive a line beginning with \"[context update]\" "
    "carrying the current date and time. Treat it as silent ambient context, not "
    "as a message from the user. Never announce, repeat, or comment on the time "
    "unless the user explicitly asks about the date or time.";

/* Format the current local time as an RFC 2822 date string. */
static void
fmt_rfc2822(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;

	if (localtime_r(&now, &tm) == NULL ||
	    strftime(buf, len, "%a, %d %b %Y %H:%M:%S %z", &tm) == 0) {
		if (len > 0)
			buf[0] = '\0';
	}
}

/*
 * Build the session-start system prompt: the base prompt, a current-time stamp,
 * and the note explaining future time updates. Returns a malloc'd string the
 * caller must free, or NULL on OOM.
 */
static char *
build_system_prompt(const char *base)
{
	char stamp[64];
	autofree char *out = NULL;
	int n;

	fmt_rfc2822(stamp, sizeof(stamp));

	n = asprintf(&out, "%s\n\ncurrent time: %s%s", base, stamp,
	    time_context_note);
	if (n < 0)
		return NULL;

	char *ret = out;
	out = NULL;
	return ret;
}

void
clm_agent_set_error(struct clm_agent *agent, const char *msg)
{
	char *dup = msg ? strdup(msg) : NULL;
	free(agent->last_error);
	agent->last_error = dup;
}

/*
 * Derive the /v1/models URL used for health probes from the chat-completions
 * base URL. The models endpoint sits beside completions, so replace a trailing
 * "/chat/completions" with "/models"; otherwise place "/models" next to the
 * last path segment. Returns a malloc'd string, or NULL on OOM.
 */
static char *
clm_derive_models_url(const char *base_url)
{
	static const char comp[] = "/chat/completions";
	static const char models[] = "/models";
	size_t blen = strlen(base_url);
	size_t clen = sizeof(comp) - 1;
	const char *slash;
	size_t prefix;
	char *out;

	if (blen >= clen && strcmp(base_url + blen - clen, comp) == 0)
		prefix = blen - clen;
	else if ((slash = strrchr(base_url, '/')) != NULL)
		prefix = (size_t)(slash - base_url);
	else
		return strdup(base_url); /* can't derive; probe as-is */

	out = malloc(prefix + sizeof(models));
	if (out == NULL)
		return NULL;
	memcpy(out, base_url, prefix);
	memcpy(out + prefix, models, sizeof(models));
	return out;
}

/*
 * llama.cpp serves GET /props at the server root, not beside the chat
 * endpoint, so this cuts base_url back to scheme://authority and appends
 * "/props". Returns a malloc'd string, or NULL if the authority can't be
 * located (e.g. no "//").
 */
static char *
clm_derive_props_url(const char *base_url)
{
	static const char props[] = "/props";
	const char *authority, *slash;
	size_t prefix;
	char *out;

	authority = strstr(base_url, "//");
	if (authority == NULL)
		return NULL;
	authority += 2; /* past "//" */

	slash = strchr(authority, '/');
	prefix = slash ? (size_t)(slash - base_url) : strlen(base_url);

	out = malloc(prefix + sizeof(props));
	if (out == NULL)
		return NULL;
	memcpy(out, base_url, prefix);
	memcpy(out + prefix, props, sizeof(props));
	return out;
}


int
clm_agent_new(const struct clm_cfg *cfg, uv_loop_t *uv, const struct clm_callbacks *cb, void *user, struct clm_agent **out)
{
	struct clm_agent *agent;
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	ASSERT_RETURN(cfg != NULL, -EINVAL);
	ASSERT_RETURN(cfg->api_key != NULL, -EINVAL);
	ASSERT_RETURN(cfg->base_url != NULL, -EINVAL);
	ASSERT_RETURN(uv != NULL, -EINVAL);

	agent = calloc(1, sizeof(*agent));
	if (agent == NULL)
		return -ENOMEM;

	agent->uv = uv;
	agent->state = CLM_STATE_IDLE;
	agent->stream = cfg->stream;
	agent->backend = cfg->backend;
	agent->max_iterations = cfg->max_iterations ? cfg->max_iterations : CLM_DEFAULT_MAX_ITERATIONS;
	clm_history_init(&agent->history);

	if (cb != NULL) {
		agent->cb_on_assistant_text = cb->on_assistant_text;
		agent->cb_on_reasoning = cb->on_reasoning;
		agent->cb_on_tool_begin = cb->on_tool_begin;
		agent->cb_on_tool_result = cb->on_tool_result;
		agent->cb_on_tool_batch = cb->on_tool_batch;
		agent->cb_on_finish_reason = cb->on_finish_reason;
		agent->cb_on_usage = cb->on_usage;
		agent->cb_on_connection = cb->on_connection;
		agent->cb_on_state = cb->on_state;
		agent->cb_on_turn_done = cb->on_turn_done;
	}
	agent->cb_user = user;

	agent->models_url = clm_derive_models_url(cfg->base_url);
	agent->props_url = clm_derive_props_url(cfg->base_url);

	r = clm_llm_new(&agent->llm, cfg->provider, cfg->api_key, cfg->base_url,
	    cfg->model ? cfg->model : "local-model");
	if (r < 0) {
		free(agent);
		return r;
	}

	{
		const char *base = cfg->system_prompt ? cfg->system_prompt
		                                       : default_system_prompt;
		autofree char *sys = build_system_prompt(base);

		if (sys == NULL || clm_history_add_system(&agent->history, sys) == NULL) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}
	agent->last_time_stamp = time(NULL);

	if (clm_tools_register_builtins(agent) < 0) {
		clm_agent_free(agent);
		return -ENOMEM;
	}

	*out = agent;
	return 0;
}

void
clm_agent_free(struct clm_agent *agent)
{
	if (agent == NULL)
		return;

	clm_tools_cancel(agent);
	clm_llm_free(agent->llm);
	clm_history_free(&agent->history);
	free(agent->last_error);
	free(agent->models_url);
	free(agent->props_url);
	free(agent->compact_body);
	clm_tools_free_registry(agent->tools, agent->tool_count);
	free(agent);
}

void
clm_agent_free_ptr(struct clm_agent **agent)
{
	if (agent && *agent) {
		clm_agent_free(*agent);
		*agent = NULL;
	}
}

enum clm_agent_state
clm_agent_get_state(const struct clm_agent *agent)
{
	return agent ? agent->state : CLM_STATE_ERROR;
}

/* Per-conversation context budget in tokens, or 0 if unknown (non-llama.cpp
 * backend, or /props not yet fetched). */
int64_t
clm_agent_get_ctx_max(const struct clm_agent *agent)
{
	return agent ? agent->ctx_max : 0;
}

const char *
clm_agent_get_last_error(const struct clm_agent *agent)
{
	if (agent == NULL || agent->last_error == NULL)
		return "";
	return agent->last_error;
}

static void clm_agent_start_turn(struct clm_agent *agent);

int
clm_agent_submit(struct clm_agent *agent, const char *prompt)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(prompt != NULL, -EINVAL);

	/*
	 * Reject only while a turn is genuinely in flight. A previous turn that
	 * finished, errored, or was cancelled must not lock out new prompts:
	 * the user should be able to just type again to recover.
	 */
	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL) {
		clm_agent_set_error(agent, "turn already in progress");
		return -EBUSY;
	}

	/*
	 * Refresh the model's sense of time on a new turn once enough has passed.
	 * The Qwen3 template forbids a non-leading system message, so this rides
	 * in a user-role message framed as ambient context; the system prompt's
	 * note tells the model to treat "[context update]" lines silently. It is
	 * appended near the newest turn (outside the cached prefix) so it does not
	 * invalidate the server's prompt cache for the earlier conversation.
	 */
	{
		time_t now = time(NULL);

		if (now - agent->last_time_stamp >= CLM_TIME_STAMP_INTERVAL) {
			char stamp[64];
			autofree char *msg = NULL;

			fmt_rfc2822(stamp, sizeof(stamp));
			if (asprintf(&msg,
			    "[context update] current time: %s\n"
			    "(automatic context, not user input; do not acknowledge)",
			    stamp) >= 0 && msg != NULL)
				(void)clm_history_add_user(&agent->history, msg);
			agent->last_time_stamp = now;
		}
	}

	if (clm_history_add_user(&agent->history, prompt) == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		return -ENOMEM;
	}

	agent->state = CLM_STATE_THINKING;
	agent->iteration = 0;

	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	clm_agent_start_turn(agent);

	return 0;
}

/* One assembled tool call accumulated from streamed deltas. */
struct stream_call {
	char *id;
	char *name;
	char *args;
	size_t args_len, args_cap;
};

struct clm_async_turn {
	struct clm_agent *agent;
	struct json_object *parsed;   /* non-streaming: the whole response */
	char *body;
	bool streaming;

	/* SSE assembly state */
	char *line;                   /* partial line across chunks */
	size_t line_len, line_cap;
	char *content;                /* assembled assistant text */
	size_t content_len, content_cap;
	struct stream_call *calls;    /* assembled tool calls, by index */
	size_t ncalls;
	char *finish_reason;          /* captured from the stream */
	struct clm_usage usage;
	bool have_usage;
};

static void
clm_async_turn_free(struct clm_async_turn *turn)
{
	size_t i;
	if (turn == NULL)
		return;
	if (turn->parsed)
		json_object_put(turn->parsed);
	free(turn->body);
	free(turn->line);
	free(turn->content);
	free(turn->finish_reason);
	for (i = 0; i < turn->ncalls; i++) {
		free(turn->calls[i].id);
		free(turn->calls[i].name);
		free(turn->calls[i].args);
	}
	free(turn->calls);
	free(turn);
}

/* Append n bytes to a growable, NUL-terminated string buffer. */
static int
sb_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n)
{
	if (*len + n + 1 > *cap) {
		size_t nc = *cap ? *cap : 256;
		char *p;
		while (nc < *len + n + 1)
			nc *= 2;
		p = realloc(*buf, nc);
		if (p == NULL)
			return -ENOMEM;
		*buf = p;
		*cap = nc;
	}
	memcpy(*buf + *len, data, n);
	*len += n;
	(*buf)[*len] = '\0';
	return 0;
}

static struct json_object *response_message(struct json_object *parsed);
static void agent_fail(struct clm_agent *agent, const char *msg, int err);

/* Turns to keep verbatim when compacting; older ones fold into the summary. */
#define CLM_COMPACT_KEEP_RECENT 4

/* Instruction appended to drive the summarization call. */
static const char *compact_prompt =
    "Summarize the conversation so far into a compact briefing that lets you "
    "continue seamlessly. Preserve: decisions made, file paths touched, "
    "commands run and their outcomes, and any open tasks or unresolved "
    "problems. Be terse and factual. Output only the summary.";

/*
 * Extract choices[0].message.content from a parsed completion into a malloc'd
 * string, or NULL. Borrowed parse; caller frees the returned copy.
 */
static char *
extract_message_content(struct json_object *parsed)
{
	struct json_object *message, *content;

	message = response_message(parsed);
	if (message == NULL)
		return NULL;
	if (!json_object_object_get_ex(message, "content", &content))
		return NULL;
	if (json_object_get_type(content) != json_type_string)
		return NULL;
	return strdup(json_object_get_string(content));
}

static void
compact_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	json_cleanup struct json_object *parsed = NULL;
	autofree char *summary = NULL;
	int status = resp ? resp->status_code : -1;

	agent->inflight = NULL;
	free(agent->compact_body);
	agent->compact_body = NULL;

	if (status != 200 || resp->body == NULL) {
		if (resp)
			clm_http_response_free(resp);
		agent_fail(agent, "compaction request failed", -EIO);
		return;
	}
	parsed = json_tokener_parse(resp->body);
	clm_http_response_free(resp);

	summary = parsed ? extract_message_content(parsed) : NULL;
	if (summary == NULL || summary[0] == '\0') {
		agent_fail(agent, "compaction produced no summary", -EIO);
		return;
	}

	if (clm_history_compact(&agent->history, summary,
	    CLM_COMPACT_KEEP_RECENT) < 0) {
		agent_fail(agent, "compaction failed", -ENOMEM);
		return;
	}

	agent->state = CLM_STATE_COMPLETE;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(0, agent->cb_user);
}

static void
compact_error_cb(int error_code, const char *error_msg, void *user)
{
	struct clm_agent *agent = user;

	agent->inflight = NULL;
	free(agent->compact_body);
	agent->compact_body = NULL;
	if (error_code == -ECANCELED)
		agent->state = CLM_STATE_COMPLETE;
	else {
		clm_agent_set_error(agent,
		    error_msg ? error_msg : "compaction request failed");
		agent->state = CLM_STATE_ERROR;
	}
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(error_code, agent->cb_user);
}

/*
 * Ask the model to summarize the conversation, then fold old turns into that
 * summary (see clm_history_compact). Manual only -- there is no auto-trigger.
 * The summarization request is the current history plus a one-off instruction;
 * it is not recorded in history, so only the compaction result persists.
 *
 * Note: compaction rewrites the prompt prefix, so the next turn cannot reuse
 * the server's prefix cache and pays a full prefill. That is acceptable for a
 * rare, user-invoked operation whose whole point is to shrink an oversized
 * context.
 */
int
clm_agent_compact(struct clm_agent *agent)
{
	json_cleanup struct json_object *req = NULL;
	struct json_object *messages, *msg, *jmodel, *jstream;
	const char *body_str;
	char *body;
	int r;

	ASSERT_RETURN(agent != NULL, -EINVAL);

	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL) {
		clm_agent_set_error(agent, "turn already in progress");
		return -EBUSY;
	}

	messages = clm_history_to_json(&agent->history);
	req = json_object_new_object();
	if (messages == NULL || req == NULL) {
		json_object_put(messages);
		return -ENOMEM;
	}

	/* Append the summarization instruction as a trailing user message. */
	msg = json_object_new_object();
	if (msg == NULL) {
		json_object_put(messages);
		return -ENOMEM;
	}
	json_object_object_add(msg, "role", json_object_new_string("user"));
	json_object_object_add(msg, "content", json_object_new_string(compact_prompt));
	json_object_array_add(messages, msg);

	jmodel = json_object_new_string(agent->llm->model);
	jstream = json_object_new_boolean(0); /* summary is a one-shot, no stream */
	if (jmodel == NULL || jstream == NULL) {
		json_object_put(messages);
		json_object_put(jmodel);
		json_object_put(jstream);
		return -ENOMEM;
	}
	json_object_object_add(req, "model", jmodel);
	json_object_object_add(req, "messages", messages);
	json_object_object_add(req, "stream", jstream);

	body_str = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
	if (body_str == NULL)
		return -ENOMEM;
	body = strdup(body_str);
	if (body == NULL)
		return -ENOMEM;

	agent->state = CLM_STATE_THINKING;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	/* curl borrows the POST body (CURLOPT_POSTFIELDS), so it must outlive the
	 * request; stash it and free it when the request completes. */
	free(agent->compact_body);
	agent->compact_body = body;

	r = clm_http_async_post(agent->uv, agent->llm->base_url,
	    agent->llm->api_key, body, compact_success_cb, compact_error_cb,
	    NULL, agent, &agent->inflight);
	if (r < 0) {
		free(agent->compact_body);
		agent->compact_body = NULL;
		agent->state = CLM_STATE_ERROR;
		return r;
	}
	return 0;
}

static void
agent_fail(struct clm_agent *agent, const char *msg, int err)
{
	clm_agent_set_error(agent, msg);
	agent->state = CLM_STATE_ERROR;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(err, agent->cb_user);
}

static enum clm_finish_reason
finish_from_str(const char *s)
{
	if (s == NULL)
		return CLM_FINISH_OTHER;
	if (strcmp(s, "stop") == 0)
		return CLM_FINISH_STOP;
	if (strcmp(s, "length") == 0)
		return CLM_FINISH_LENGTH;
	if (strcmp(s, "tool_calls") == 0)
		return CLM_FINISH_TOOL_CALLS;
	if (strcmp(s, "content_filter") == 0)
		return CLM_FINISH_CONTENT_FILTER;
	return CLM_FINISH_OTHER;
}

static void
emit_finish(struct clm_agent *agent, const char *reason)
{
	if (reason != NULL && agent->cb_on_finish_reason)
		agent->cb_on_finish_reason(finish_from_str(reason), agent->cb_user);
}

/* Read usage/timings from a response object. Returns true if usage present. */
static bool
extract_usage(struct json_object *root, struct clm_usage *out)
{
	struct json_object *u = NULL, *t = NULL, *v = NULL;

	if (!json_object_object_get_ex(root, "usage", &u) ||
	    json_object_get_type(u) != json_type_object)
		return false;
	memset(out, 0, sizeof(*out));
	if (json_object_object_get_ex(u, "prompt_tokens", &v))
		out->prompt_tokens = json_object_get_int(v);
	if (json_object_object_get_ex(u, "completion_tokens", &v))
		out->completion_tokens = json_object_get_int(v);
	if (json_object_object_get_ex(u, "total_tokens", &v))
		out->total_tokens = json_object_get_int(v);
	if (json_object_object_get_ex(root, "timings", &t) &&
	    json_object_get_type(t) == json_type_object &&
	    json_object_object_get_ex(t, "predicted_per_second", &v))
		out->tokens_per_sec = json_object_get_double(v);
	return true;
}

static void
emit_usage(struct clm_agent *agent, const struct clm_usage *usage)
{
	if (agent->cb_on_usage)
		agent->cb_on_usage(usage, agent->cb_user);
}

/*
 * Finalize a completed model message: dispatch tool calls if present, else
 * record the assistant text and end the turn. tool_calls is borrowed (the
 * caller frees it after this returns; dispatch copies what it needs). When
 * streamed is true, on_assistant_text has already fired per delta, so the
 * text is recorded without re-firing.
 */
static void
agent_finish(struct clm_agent *agent, struct json_object *tool_calls,
    const char *content, bool streamed)
{
	if (tool_calls != NULL && json_object_get_type(tool_calls) == json_type_array
	    && json_object_array_length(tool_calls) > 0) {
		int r;
		agent->state = CLM_STATE_CALLING_TOOL;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		r = clm_tools_dispatch(agent, tool_calls);
		if (r < 0)
			agent_fail(agent, "failed to dispatch tools", r);
		return;
	}

	if (content != NULL) {
		if (clm_history_add_assistant_text(&agent->history, content) == NULL) {
			agent_fail(agent, "out of memory", -ENOMEM);
			return;
		}
		if (!streamed && agent->cb_on_assistant_text)
			agent->cb_on_assistant_text(content, agent->cb_user);
	}

	agent->state = CLM_STATE_COMPLETE;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(0, agent->cb_user);
}

/* Build a json tool_calls array from streamed accumulators, or NULL. */
static struct json_object *
stream_build_tool_calls(struct clm_async_turn *turn)
{
	json_cleanup struct json_object *arr = NULL;
	struct json_object *ret;
	size_t i;

	if (turn->ncalls == 0)
		return NULL;
	arr = json_object_new_array();
	if (arr == NULL)
		return NULL;

	for (i = 0; i < turn->ncalls; i++) {
		struct json_object *call, *func;
		if (turn->calls[i].name == NULL)
			continue;
		call = json_object_new_object();
		if (call == NULL)
			return NULL;
		json_object_array_add(arr, call);
		json_object_object_add(call, "id",
		    json_object_new_string(turn->calls[i].id ? turn->calls[i].id : ""));
		json_object_object_add(call, "type", json_object_new_string("function"));
		func = json_object_new_object();
		if (func == NULL)
			return NULL;
		json_object_object_add(call, "function", func);
		json_object_object_add(func, "name", json_object_new_string(turn->calls[i].name));
		json_object_object_add(func, "arguments",
		    json_object_new_string(turn->calls[i].args ? turn->calls[i].args : "{}"));
	}

	if (json_object_array_length(arr) == 0)
		return NULL;
	ret = arr;
	arr = NULL;
	return ret;
}

static void
stream_finalize(struct clm_async_turn *turn)
{
	struct clm_agent *agent = turn->agent;
	json_cleanup struct json_object *tool_calls = stream_build_tool_calls(turn);

	emit_finish(agent, turn->finish_reason);
	if (turn->have_usage)
		emit_usage(agent, &turn->usage);
	agent_finish(agent, tool_calls, turn->content, true);
	clm_async_turn_free(turn);
}

static void
clm_http_success_cb_wrapper(struct clm_http_response *resp, void *user)
{
	struct clm_async_turn *turn = (struct clm_async_turn *)user;
	struct clm_agent *agent = turn->agent;
	struct json_object *message, *content = NULL, *tool_calls = NULL;
	int status = resp ? resp->status_code : -1;

	agent->inflight = NULL; /* request has completed */
	clm_debug("status=%d streaming=%d", status, turn->streaming);

	if (status != 200) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "HTTP %d", status);
		if (resp)
			clm_http_response_free(resp);
		clm_async_turn_free(turn);
		agent_fail(agent, buf, -EIO);
		return;
	}

	if (turn->streaming) {
		if (resp)
			clm_http_response_free(resp);
		stream_finalize(turn);
		return;
	}

	turn->parsed = resp && resp->body ? json_tokener_parse(resp->body) : NULL;
	if (resp)
		clm_http_response_free(resp);

	if (turn->parsed == NULL) {
		clm_async_turn_free(turn);
		agent_fail(agent, "could not parse llm response", -EIO);
		return;
	}
	message = response_message(turn->parsed);
	if (message == NULL) {
		clm_async_turn_free(turn);
		agent_fail(agent, "could not extract message from llm response", -EIO);
		return;
	}

	{
		struct json_object *choices, *choice0, *jfinish = NULL, *jreason = NULL;
		struct clm_usage usage;
		if (json_object_object_get_ex(turn->parsed, "choices", &choices) &&
		    (choice0 = json_object_array_get_idx(choices, 0)) != NULL &&
		    json_object_object_get_ex(choice0, "finish_reason", &jfinish) &&
		    json_object_get_type(jfinish) == json_type_string)
			jreason = jfinish;
		emit_finish(agent, jreason ? json_object_get_string(jreason) : NULL);
		if (extract_usage(turn->parsed, &usage))
			emit_usage(agent, &usage);
	}

	/* Non-streaming reasoning, if the model exposes a think channel. */
	{
		struct json_object *jreason = NULL;
		if ((json_object_object_get_ex(message, "reasoning_content", &jreason) ||
		    json_object_object_get_ex(message, "reasoning", &jreason)) &&
		    json_object_get_type(jreason) == json_type_string &&
		    agent->cb_on_reasoning)
			agent->cb_on_reasoning(json_object_get_string(jreason), agent->cb_user);
	}

	json_object_object_get_ex(message, "content", &content);
	json_object_object_get_ex(message, "tool_calls", &tool_calls);
	agent_finish(agent, tool_calls,
	    content ? json_object_get_string(content) : NULL, false);
	clm_async_turn_free(turn);
}

/* ------------------------------------------------------------------ */
/* SSE streaming parser                                                */
/* ------------------------------------------------------------------ */

static struct stream_call *
stream_get_call(struct clm_async_turn *turn, size_t index)
{
	if (index >= turn->ncalls) {
		struct stream_call *p = realloc(turn->calls,
		    (index + 1) * sizeof(*turn->calls));
		if (p == NULL)
			return NULL;
		turn->calls = p;
		while (turn->ncalls <= index)
			memset(&turn->calls[turn->ncalls++], 0, sizeof(*turn->calls));
	}
	return &turn->calls[index];
}

static void
stream_merge_tool_calls(struct clm_async_turn *turn, struct json_object *deltas)
{
	size_t i, n = json_object_array_length(deltas);

	for (i = 0; i < n; i++) {
		struct json_object *d = json_object_array_get_idx(deltas, i);
		struct json_object *jidx = NULL, *jid = NULL, *func = NULL, *jname = NULL, *jargs = NULL;
		struct stream_call *call;
		int index = (int)i;

		if (d == NULL)
			continue;
		if (json_object_object_get_ex(d, "index", &jidx))
			index = json_object_get_int(jidx);
		if (index < 0)
			continue;
		call = stream_get_call(turn, (size_t)index);
		if (call == NULL)
			continue;

		if (json_object_object_get_ex(d, "id", &jid) && call->id == NULL)
			call->id = strdup(json_object_get_string(jid));
		if (json_object_object_get_ex(d, "function", &func)) {
			if (json_object_object_get_ex(func, "name", &jname) && call->name == NULL)
				call->name = strdup(json_object_get_string(jname));
			if (json_object_object_get_ex(func, "arguments", &jargs)) {
				const char *frag = json_object_get_string(jargs);
				(void)sb_append(&call->args, &call->args_len,
				    &call->args_cap, frag, strlen(frag));
			}
		}
	}
}

/* Process one complete SSE line (NUL-terminated, no newline). */
static void
stream_handle_line(struct clm_async_turn *turn)
{
	struct clm_agent *agent = turn->agent;
	json_cleanup struct json_object *obj = NULL;
	struct json_object *choices, *choice, *delta, *content = NULL, *tcs = NULL;
	const char *line = turn->line ? turn->line : "";
	const char *payload;

	/*
	 * If the turn was cancelled, clm_agent_cancel has already cleared
	 * inflight and started tearing the request down; drop any buffered or
	 * in-transit stream data so a cancelled reply stops rendering at once.
	 */
	if (agent->inflight == NULL)
		return;

	if (turn->line_len > 0 && line[turn->line_len - 1] == '\r')
		turn->line[--turn->line_len] = '\0';
	if (turn->line_len == 0 || strncmp(line, "data:", 5) != 0)
		return;
	payload = line + 5;
	while (*payload == ' ')
		payload++;
	if (strcmp(payload, "[DONE]") == 0)
		return;

	obj = json_tokener_parse(payload);
	if (obj == NULL)
		return;

	/* Usage/timings arrive in a trailing chunk with an empty choices array
	 * (when stream_options.include_usage is set), so check it first. */
	if (!turn->have_usage && extract_usage(obj, &turn->usage))
		turn->have_usage = true;

	if (!json_object_object_get_ex(obj, "choices", &choices) ||
	    json_object_get_type(choices) != json_type_array)
		return;
	choice = json_object_array_get_idx(choices, 0);
	if (choice == NULL)
		return;

	{
		struct json_object *jfinish = NULL;
		if (json_object_object_get_ex(choice, "finish_reason", &jfinish) &&
		    json_object_get_type(jfinish) == json_type_string) {
			free(turn->finish_reason);
			turn->finish_reason = strdup(json_object_get_string(jfinish));
		}
	}

	if (!json_object_object_get_ex(choice, "delta", &delta))
		return;

	if (json_object_object_get_ex(delta, "content", &content) &&
	    json_object_get_type(content) == json_type_string) {
		const char *text = json_object_get_string(content);
		size_t tlen = strlen(text);
		if (tlen > 0) {
			(void)sb_append(&turn->content, &turn->content_len,
			    &turn->content_cap, text, tlen);
			if (agent->cb_on_assistant_text)
				agent->cb_on_assistant_text(text, agent->cb_user);
		}
	}

	/* Reasoning / think channel, for models that emit one. */
	{
		struct json_object *jr = NULL;
		if ((json_object_object_get_ex(delta, "reasoning_content", &jr) ||
		    json_object_object_get_ex(delta, "reasoning", &jr)) &&
		    json_object_get_type(jr) == json_type_string) {
			const char *rt = json_object_get_string(jr);
			if (rt[0] != '\0' && agent->cb_on_reasoning)
				agent->cb_on_reasoning(rt, agent->cb_user);
		}
	}

	if (json_object_object_get_ex(delta, "tool_calls", &tcs) &&
	    json_object_get_type(tcs) == json_type_array)
		stream_merge_tool_calls(turn, tcs);
}

static void
clm_http_data_cb_wrapper(const char *data, size_t len, void *user)
{
	struct clm_async_turn *turn = (struct clm_async_turn *)user;
	size_t i;

	for (i = 0; i < len; i++) {
		if (data[i] == '\n') {
			stream_handle_line(turn);
			turn->line_len = 0;
			if (turn->line != NULL)
				turn->line[0] = '\0';
		} else {
			(void)sb_append(&turn->line, &turn->line_len,
			    &turn->line_cap, &data[i], 1);
		}
	}
}

static void
clm_http_error_cb_wrapper(int error_code, const char *error_msg, void *user)
{
	struct clm_async_turn *turn = (struct clm_async_turn *)user;
	struct clm_agent *agent = turn->agent;

	agent->inflight = NULL; /* request has completed (or was cancelled) */

	/*
	 * A user cancel is not an error: land in a clean, submittable state and
	 * leave no error string, so the status bar does not show "error" and the
	 * next prompt just works.
	 */
	if (error_code == -ECANCELED) {
		agent->state = CLM_STATE_COMPLETE;
	} else {
		clm_agent_set_error(agent,
		    error_msg ? error_msg : "http request failed");
		agent->state = CLM_STATE_ERROR;
	}
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	clm_async_turn_free(turn);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(error_code, agent->cb_user);
}

/* GET /props completed: parse llama.cpp context info; ignore failures (the
 * feature is best-effort and only meaningful for llama.cpp backends). */
static void
props_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	int64_t ctx = 0;

	if (resp != NULL && resp->status_code >= 200 && resp->status_code < 300 &&
	    resp->body != NULL && clm_parse_props(resp->body, &ctx) == 0) {
		agent->backend = CLM_BACKEND_LLAMACPP; /* /props => llama.cpp */
		agent->ctx_max = ctx;
	}
	if (resp)
		clm_http_response_free(resp);
}

static void
props_error_cb(int error_code, const char *error_msg, void *user)
{
	(void)error_code;
	(void)error_msg;
	(void)user; /* no /props (not llama.cpp, or old build): leave ctx unknown */
}

/*
 * Probe GET /props to learn the context window and confirm a llama.cpp
 * backend. Skipped when the caller pinned a non-llama.cpp backend, or when the
 * url could not be derived.
 */
static void
clm_agent_fetch_props(struct clm_agent *agent)
{
	if (agent->props_url == NULL)
		return;
	if (agent->backend != CLM_BACKEND_GENERIC &&
	    agent->backend != CLM_BACKEND_LLAMACPP)
		return;
	(void)clm_http_async_post(agent->uv, agent->props_url,
	    agent->llm->api_key, NULL, props_success_cb, props_error_cb, NULL,
	    agent, NULL);
}

/* Health probe completed (GET /v1/models): 2xx is online, anything else is
 * offline. user is the agent (not a turn). */
static void
health_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	int status = resp ? resp->status_code : -1;

	if (agent->cb_on_connection) {
		if (status >= 200 && status < 300) {
			agent->cb_on_connection(CLM_CONN_ONLINE, NULL,
			    agent->cb_user);
			/* Learn the context window once, now that it's up. */
			if (agent->ctx_max == 0)
				clm_agent_fetch_props(agent);
		} else {
			char detail[64];
			(void)snprintf(detail, sizeof(detail), "HTTP %d",
			    status);
			agent->cb_on_connection(CLM_CONN_OFFLINE, detail,
			    agent->cb_user);
		}
	}
	if (resp)
		clm_http_response_free(resp);
}

static void
health_error_cb(int error_code, const char *error_msg, void *user)
{
	struct clm_agent *agent = user;

	(void)error_code;
	if (agent->cb_on_connection)
		agent->cb_on_connection(CLM_CONN_OFFLINE,
		    error_msg ? error_msg : "unreachable", agent->cb_user);
}

int
clm_agent_check_connection(struct clm_agent *agent)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(agent->models_url != NULL, -EINVAL);

	if (agent->cb_on_connection)
		agent->cb_on_connection(CLM_CONN_CHECKING, NULL, agent->cb_user);

	/* NULL body => GET. user is the agent, distinct from turn requests;
	 * out_req is NULL so this probe is not tracked for cancellation. */
	return clm_http_async_post(agent->uv, agent->models_url,
	    agent->llm->api_key, NULL, health_success_cb, health_error_cb, NULL,
	    agent, NULL);
}

int
clm_agent_cancel(struct clm_agent *agent)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);

	if (agent->inflight != NULL) {
		/* Waiting on the model: abort the request. Its error callback
		 * fires on_turn_done(-ECANCELED) and clears inflight. */
		struct clm_http_request *req = agent->inflight;
		agent->inflight = NULL;
		clm_http_async_cancel(req);
		return 0;
	}
	if (agent->active_batch != NULL) {
		/* Running tools: signal them to abort; when the batch finishes
		 * unwinding, clm_agent_tools_done ends the turn as cancelled. */
		agent->cancelling = true;
		clm_tools_cancel(agent);
		return 0;
	}
	return -EINVAL; /* nothing in flight */
}

static void
clm_agent_start_turn(struct clm_agent *agent)
{
	json_cleanup struct json_object *req = NULL;
	struct json_object *messages = NULL;
	struct json_object *tools = NULL;
	struct json_object *jmodel = NULL;
	struct json_object *jstream = NULL;
	struct clm_async_turn *turn;
	const char *body_str;
	char *body;

	messages = clm_history_to_json(&agent->history);
	tools = clm_tools_build_schema(agent);
	if (messages == NULL || tools == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	req = json_object_new_object();
	if (req == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	jmodel = json_object_new_string(agent->llm->model);
	if (jmodel == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}
	json_object_object_add(req, "model", jmodel);

	json_object_object_add(req, "messages", messages);

	jstream = json_object_new_boolean(agent->stream ? 1 : 0);
	if (jstream == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}
	json_object_object_add(req, "stream", jstream);

	/* Ask the server to include token usage in the final stream chunk. */
	if (agent->stream) {
		struct json_object *so = json_object_new_object();
		if (so != NULL) {
			struct json_object *inc = json_object_new_boolean(1);
			if (inc != NULL)
				json_object_object_add(so, "include_usage", inc);
			json_object_object_add(req, "stream_options", so);
		}
	}

	json_object_object_add(req, "tools", tools);

	body_str = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
	if (body_str == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	body = strdup(body_str);
	if (body == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	turn = calloc(1, sizeof(struct clm_async_turn));
	if (turn == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	turn->agent = agent;
	turn->body = body;
	turn->streaming = agent->stream;
	body = NULL;

	clm_debug("posting body: %s", turn->body);

	clm_http_async_post(agent->uv, agent->llm->base_url, agent->llm->api_key,
			    turn->body, clm_http_success_cb_wrapper, clm_http_error_cb_wrapper,
			    agent->stream ? clm_http_data_cb_wrapper : NULL, turn,
			    &agent->inflight);
}

/*
 * Reach into a parsed completion response and return choices[0].message,
 * or NULL. The returned object is borrowed from parsed.
 */
static struct json_object *
response_message(struct json_object *parsed)
{
	struct json_object *choices = NULL, *choice0 = NULL, *message = NULL;

	if (!json_object_object_get_ex(parsed, "choices", &choices))
		return NULL;
	if (json_object_get_type(choices) != json_type_array)
		return NULL;
	choice0 = json_object_array_get_idx(choices, 0);
	if (choice0 == NULL)
		return NULL;
	if (!json_object_object_get_ex(choice0, "message", &message))
		return NULL;
	return message;
}

void
clm_agent_tools_done(struct clm_agent *agent, int status)
{
	if (agent->cancelling) { /* Escape hit while tools were running */
		agent->cancelling = false;
		/* A cancel is not an error: land clean and submittable. */
		agent->state = CLM_STATE_COMPLETE;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ECANCELED, agent->cb_user);
		return;
	}

	if (status < 0) {
		clm_agent_set_error(agent, "tool execution failed");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(status, agent->cb_user);
		return;
	}

	if (++agent->iteration >= agent->max_iterations) {
		clm_agent_set_error(agent, "max iterations reached");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-E2BIG, agent->cb_user);
		return;
	}

	agent->state = CLM_STATE_THINKING;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	clm_agent_start_turn(agent);
}
