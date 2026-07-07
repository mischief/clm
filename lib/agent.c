// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>

#include "clm/agent.h"
#include "clm/http.h"
#include "clm/host.h"
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

/*
 * Start an HTTP request through the host transport. All of the agent's requests
 * use the configured API key and no extra headers or client suffix, so this
 * wraps the common shape; callers vary only url/body/callbacks/user/out.
 */
static int
agent_http_post(struct clm_agent *agent, const char *url, const char *body,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	struct clm_http_req req = {
		.url = url,
		.api_key = agent->llm->api_key,
		.body = body,
		.headers = NULL,
		.client_suffix = NULL,
	};
	return agent->host->http_post(agent->host->ctx, &req, success, error,
	    data, user, out);
}

int
clm_agent_new(const struct clm_cfg *cfg, struct clm_host *host, const struct clm_callbacks *cb, void *user, struct clm_agent **out)
{
	struct clm_agent *agent;
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	ASSERT_RETURN(cfg != NULL, -EINVAL);
	ASSERT_RETURN(cfg->api_key != NULL, -EINVAL);
	ASSERT_RETURN(cfg->base_url != NULL, -EINVAL);
	ASSERT_RETURN(host != NULL, -EINVAL);
	ASSERT_RETURN(host->http_post != NULL, -EINVAL);

	agent = calloc(1, sizeof(*agent));
	if (agent == NULL)
		return -ENOMEM;

	agent->host = host;
	agent->state = CLM_STATE_IDLE;
	agent->stream = cfg->stream;
	agent->backend = cfg->backend;
	agent->max_iterations = cfg->max_iterations; /* 0 = unlimited */
	clm_history_init(&agent->history);
	TAILQ_INIT(&agent->tools);

	if (cb != NULL) {
		agent->cb_on_assistant_text = cb->on_assistant_text;
		agent->cb_on_reasoning = cb->on_reasoning;
		agent->cb_on_tool_begin = cb->on_tool_begin;
		agent->cb_on_permission = cb->on_permission;
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

		if (sys == NULL ||
		    clm_history_add_system(&agent->history, sys, agent->compressor) == NULL) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}
	agent->last_time_stamp = time(NULL);

	if (clm_tools_register_builtins(agent) < 0) {
		clm_agent_free(agent);
		return -ENOMEM;
	}

	/* Tool dispatch rate limiter: 1 token/sec refill, burst of 8. */
	if (clm_ratelimit_new(&agent->tool_rl, 1, 8) < 0) {
		clm_agent_free(agent);
		return -ENOMEM;
	}

	/* LLM request rate limiter, estimated-token bucket (see llm_rl in
	 * internal.h for why this exists separately from tool_rl). Defaults
	 * are conservative on purpose: this has to work across very
	 * different backend tiers (a local llama.cpp server has no real
	 * limit; hosted APIs vary widely), and getting briefly parked is a
	 * much smaller problem than the 429 burst it guards against.
	 * Overridable per provider via cfg. */
	{
		int64_t rps = cfg->rate_tokens_per_sec > 0 ? cfg->rate_tokens_per_sec : 2000;
		int64_t burst = cfg->rate_burst > 0 ? cfg->rate_burst : 30000;
		if (clm_ratelimit_new(&agent->llm_rl, (size_t)rps, (size_t)burst) < 0) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}

	/* Apply provider overrides from config */
	if (cfg->context_size > 0)
		agent->ctx_max = cfg->context_size;
	if (cfg->autocompact_pct > 0)
		agent->autocompact_pct = cfg->autocompact_pct;

	agent->volatile_tools = cfg->volatile_tools;

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
	free(agent->pending_notify);
	clm_tools_free_registry(&agent->tools);
	clm_ratelimit_free(agent->tool_rl);
	if (agent->llm_rl_timer != NULL && agent->host != NULL &&
	    agent->host->timer_cancel != NULL)
		agent->host->timer_cancel(agent->llm_rl_timer);
	clm_ratelimit_free(agent->llm_rl);
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

void
clm_agent_set_compressor(struct clm_agent *agent, const struct clm_compressor *cz)
{
	if (agent == NULL)
		return;
	agent->compressor = cz;
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

/* Auto-compact threshold. Only meaningful as a percentage when ctx_max is
 * known -- from probing GET /props against a llama.cpp backend, or from an
 * explicit context_size provider override -- which hosted backends
 * (Anthropic, OpenAI, etc.) don't expose. Against those, ctx_max stays 0,
 * so CLM_AUTOCOMPACT_FALLBACK_TOKENS below is used instead of a percentage
 * -- see clm_agent_over_autocompact_threshold(). */
#define CLM_AUTOCOMPACT_PCT 70

/*
 * Absolute token fallback for backends with no known ctx_max (see above).
 * This exists purely because a real billing incident happened without it:
 * running an agent against a paid API with no working compaction meant
 * every turn resent the entire accumulated history, and that cost real
 * money fast with no ceiling in place. Picked to be comfortably small for
 * a paid API rather than tuned to any specific model's real context
 * window -- the whole point is capping *cost*, not maximizing context
 * use, when the actual window size is unknown. */
#define CLM_AUTOCOMPACT_FALLBACK_TOKENS 100000

/*
 * True if context usage (tracked in emit_usage() below) is at/above the
 * autocompact threshold. Shared by clm_agent_tools_done()'s mid-chain
 * check and tui.c's own end-of-turn check, so both use exactly the same
 * calc instead of tui.c keeping a second copy in sync by hand.
 */
bool
clm_agent_over_autocompact_threshold(const struct clm_agent *agent)
{
	if (agent == NULL || agent->ctx_used <= 0)
		return false;
	if (agent->ctx_max <= 0)
		return agent->ctx_used >= CLM_AUTOCOMPACT_FALLBACK_TOKENS;
	int pct = agent->autocompact_pct > 0 ? agent->autocompact_pct : CLM_AUTOCOMPACT_PCT;
	return (agent->ctx_used * 100) / agent->ctx_max >= pct;
}

const char *
clm_agent_get_last_error(const struct clm_agent *agent)
{
	if (agent == NULL || agent->last_error == NULL)
		return "";
	return agent->last_error;
}

bool
clm_agent_take_mid_chain_compact_error(struct clm_agent *agent)
{
	bool failed;

	if (agent == NULL)
		return false;

	failed = agent->mid_chain_compact_failed;
	agent->mid_chain_compact_failed = false;
	return failed;
}

bool
clm_agent_take_mid_chain_compact_started(struct clm_agent *agent)
{
	bool started;

	if (agent == NULL)
		return false;

	started = agent->mid_chain_compact_started;
	agent->mid_chain_compact_started = false;
	return started;
}

bool
clm_agent_take_mid_chain_compact_succeeded(struct clm_agent *agent)
{
	bool succeeded;

	if (agent == NULL)
		return false;

	succeeded = agent->mid_chain_compact_succeeded;
	agent->mid_chain_compact_succeeded = false;
	return succeeded;
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
				(void)clm_history_add_user(&agent->history, msg,
				    agent->compressor);
			agent->last_time_stamp = now;
		}
	}

	if (clm_history_add_user(&agent->history, prompt, agent->compressor) == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		return -ENOMEM;
	}

	agent->state = CLM_STATE_THINKING;
	agent->iteration = 0;
	agent->cancelling = false; /* fresh turn: clear any prior cancel */

	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	clm_agent_start_turn(agent);

	return 0;
}

/*
 * Fire cb_on_turn_done for the turn that just landed, then submit any text
 * queued by clm_agent_notify() while it was in flight. Every call site that
 * ends a real turn (as opposed to the mid-chain compact resume path, which
 * deliberately does not fire cb_on_turn_done at all) routes through here so
 * a background notification queued mid-turn is never dropped and never
 * jumps ahead of the turn already in progress.
 *
 * Safe to call clm_agent_submit() from here: every call site reaches this
 * function only after agent->state has already been set to CLM_STATE_COMPLETE
 * or CLM_STATE_ERROR (never THINKING/CALLING_TOOL), which is exactly the
 * condition clm_agent_submit requires to accept a new prompt.
 */
static void
agent_turn_done(struct clm_agent *agent, int status)
{
	autofree char *notify = agent->pending_notify;
	agent->pending_notify = NULL;

	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(status, agent->cb_user);

	if (notify != NULL)
		(void)clm_agent_submit(agent, notify);
}

int
clm_agent_notify(struct clm_agent *agent, const char *text)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(text != NULL, -EINVAL);

	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL) {
		char *joined = NULL;

		if (agent->pending_notify == NULL) {
			joined = strdup(text);
		} else if (asprintf(&joined, "%s\n\n%s", agent->pending_notify,
		    text) < 0) {
			return -ENOMEM; /* joined's contents are undefined on failure */
		}
		if (joined == NULL)
			return -ENOMEM;
		free(agent->pending_notify);
		agent->pending_notify = joined;
		return 0;
	}

	return clm_agent_submit(agent, text);
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
	cJSON *parsed;   /* non-streaming: the whole response */
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
		cJSON_Delete(turn->parsed);
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

static cJSON *response_message(cJSON *parsed);
static void agent_fail(struct clm_agent *agent, const char *msg, int err);

/* Turns to keep verbatim when compacting; older ones fold into the summary. */
#define CLM_COMPACT_KEEP_RECENT 2

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
extract_message_content(cJSON *parsed)
{
	cJSON *message, *content;

	message = response_message(parsed);
	if (message == NULL)
		return NULL;
	content = cJSON_GetObjectItemCaseSensitive(message, "content");
	if (content == NULL || !cJSON_IsString(content))
		return NULL;
	return strdup(cJSON_GetStringValue(content));
}

static void
compact_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	json_cleanup cJSON *parsed = NULL;
	autofree char *summary = NULL;
	int status = resp ? resp->status_code : -1;
	bool resume = agent->compact_resume_chain;
	agent->compact_resume_chain = false;

	agent->inflight = NULL;
	free(agent->compact_body);
	agent->compact_body = NULL;

	if (status != 200 || resp->body == NULL) {
		if (resp)
			clm_http_response_free(resp);
		if (resume) {
			/* Mid-chain: not fatal, just didn't shrink anything --
			 * continue the interrupted chain as-is rather than
			 * landing the whole turn in an error state over a
			 * compaction hiccup. */
			clm_agent_set_error(agent, "compaction request failed");
			agent->mid_chain_compact_failed = true;
			clm_agent_start_turn(agent);
			return;
		}
		agent_fail(agent, "compaction request failed", -EIO);
		return;
	}
	parsed = cJSON_Parse(resp->body);
	clm_debug("compact response body: %s", resp->body);
	clm_http_response_free(resp);

	summary = parsed ? extract_message_content(parsed) : NULL;
	if (summary == NULL || summary[0] == '\0') {
		if (resume) {
			clm_agent_set_error(agent, "compaction produced no summary");
			agent->mid_chain_compact_failed = true;
			clm_agent_start_turn(agent);
			return;
		}
		agent_fail(agent, "compaction produced no summary", -EIO);
		return;
	}

	{
		int folded = clm_history_compact(&agent->history, summary,
		    CLM_COMPACT_KEEP_RECENT, agent->compressor);
		/* folded == 0 is failure here, not success: the history had no
		 * valid cut point, so nothing shrank and the summary we just
		 * paid a full-history LLM call for was discarded. Reporting it
		 * as success is what caused the "compact forever" loop -- the
		 * context stayed over threshold and every subsequent tool
		 * round-trip re-triggered another futile summarize call. */
		if (folded <= 0) {
			const char *why = folded < 0 ? "compaction failed"
			    : "compaction made no progress";
			if (resume) {
				clm_agent_set_error(agent, why);
				agent->mid_chain_compact_failed = true;
				clm_agent_start_turn(agent);
				return;
			}
			agent_fail(agent, why,
			    folded < 0 ? folded : -EAGAIN);
			return;
		}
	}

	if (resume) {
		/* Not a real turn ending -- resume the interrupted tool
		 * chain's next LLM call directly instead of reporting
		 * cb_on_turn_done, which the caller would otherwise mistake
		 * for the conversational turn actually finishing.
		 * This is the success path, so clear any stale failure flag
		 * from an earlier mid-chain attempt. */
		agent->mid_chain_compact_failed = false;
		agent->mid_chain_compact_succeeded = true;
		/* The compact request's own usage callback just set ctx_used
		 * to the (large) token count of the compaction call itself --
		 * that's stale and no longer reflects the actual conversation
		 * size (which just shrank). Clear it so the next tools_done
		 * check doesn't immediately re-trigger compaction on the
		 * stale reading; the resumed turn's own response will report
		 * the real (smaller) count. */
		agent->ctx_used = 0;
		agent->state = CLM_STATE_THINKING;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		clm_agent_start_turn(agent);
		return;
	}

	agent->state = CLM_STATE_COMPLETE;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	agent_turn_done(agent, 0);
}

static void
compact_error_cb(int error_code, const char *error_msg, void *user)
{
	struct clm_agent *agent = user;
	bool resume = agent->compact_resume_chain;
	agent->compact_resume_chain = false;

	agent->inflight = NULL;
	free(agent->compact_body);
	agent->compact_body = NULL;

	if (resume && error_code != -ECANCELED) {
		/* Mid-chain, non-cancel failure: same "not fatal, keep going"
		 * handling as the success path above -- don't land the whole
		 * turn in CLM_STATE_ERROR over a compaction hiccup. */
		clm_agent_set_error(agent,
		    error_msg ? error_msg : "compaction request failed");
		agent->mid_chain_compact_failed = true;
		clm_agent_start_turn(agent);
		return;
	}

	if (error_code == -ECANCELED)
		agent->state = CLM_STATE_COMPLETE;
	else {
		clm_agent_set_error(agent,
		    error_msg ? error_msg : "compaction request failed");
		agent->state = CLM_STATE_ERROR;
	}
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	agent_turn_done(agent, error_code);
}

/*
 * Ask the model to summarize the conversation, then fold old turns into that
 * summary (see clm_history_compact). Triggered explicitly by a caller, or
 * internally from clm_agent_tools_done() when usage crosses the autocompact
 * threshold mid-chain (see compact_resume_chain in internal.h).
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
	json_cleanup cJSON *req = NULL;
	cJSON *messages, *msg, *jmodel, *jstream;
	autofree char *body_str = NULL;
	char *body;
	int r;

	ASSERT_RETURN(agent != NULL, -EINVAL);

	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL) {
		clm_agent_set_error(agent, "turn already in progress");
		return -EBUSY;
	}

	messages = clm_history_to_json(&agent->history, agent->compressor);
	req = cJSON_CreateObject();
	if (messages == NULL || req == NULL) {
		cJSON_Delete(messages);
		return -ENOMEM;
	}

	/* Append the summarization instruction as a trailing user message. */
	msg = cJSON_CreateObject();
	if (msg == NULL) {
		cJSON_Delete(messages);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
	cJSON_AddItemToObject(msg, "content", cJSON_CreateString(compact_prompt));
	cJSON_AddItemToArray(messages, msg);

	jmodel = cJSON_CreateString(agent->llm->model);
	jstream = cJSON_CreateBool(0); /* summary is a one-shot, no stream */
	if (jmodel == NULL || jstream == NULL) {
		cJSON_Delete(messages);
		cJSON_Delete(jmodel);
		cJSON_Delete(jstream);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(req, "model", jmodel);
	cJSON_AddItemToObject(req, "messages", messages);
	cJSON_AddItemToObject(req, "stream", jstream);

	body_str = cJSON_PrintUnformatted(req);
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

	r = agent_http_post(agent, agent->llm->base_url, body,
	    compact_success_cb, compact_error_cb, NULL, agent, &agent->inflight);
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
	agent_turn_done(agent, err);
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
extract_usage(cJSON *root, struct clm_usage *out)
{
	cJSON *u, *t, *v;

	u = cJSON_GetObjectItemCaseSensitive(root, "usage");
	if (u == NULL || !cJSON_IsObject(u))
		return false;
	memset(out, 0, sizeof(*out));
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens")) != NULL)
		out->prompt_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "completion_tokens")) != NULL)
		out->completion_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "total_tokens")) != NULL)
		out->total_tokens = (int)cJSON_GetNumberValue(v);
	t = cJSON_GetObjectItemCaseSensitive(root, "timings");
	if (t != NULL && cJSON_IsObject(t) &&
	    (v = cJSON_GetObjectItemCaseSensitive(t, "predicted_per_second")) != NULL)
		out->tokens_per_sec = cJSON_GetNumberValue(v);
	return true;
}

static void
emit_usage(struct clm_agent *agent, const struct clm_usage *usage)
{
	/* Tokens carried into the next turn's prompt -- same calc tui.c's own
	 * cb_usage does for its status-bar gauge, kept here too now so
	 * clm_agent_tools_done() can check the autocompact threshold itself
	 * mid-chain without depending on a UI layer to track it. */
	agent->ctx_used = (int64_t)usage->prompt_tokens + usage->completion_tokens;
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
agent_finish(struct clm_agent *agent, cJSON *tool_calls,
    const char *content, bool streamed)
{
	if (tool_calls != NULL && cJSON_IsArray(tool_calls)
	    && cJSON_GetArraySize(tool_calls) > 0) {
		int r;
		if (content != NULL && content[0] != '\0')
			clm_debug("[think] %.*s", (int)(strlen(content) > 200 ? 200 : strlen(content)), content);
		agent->state = CLM_STATE_CALLING_TOOL;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		r = clm_tools_dispatch(agent, tool_calls);
		if (r < 0)
			agent_fail(agent, "failed to dispatch tools", r);
		return;
	}

	if (content != NULL) {
		clm_debug("[think] %.*s", (int)(strlen(content) > 200 ? 200 : strlen(content)), content);
		if (clm_history_add_assistant_text(&agent->history, content,
		    agent->compressor) == NULL) {
			agent_fail(agent, "out of memory", -ENOMEM);
			return;
		}
		if (!streamed && agent->cb_on_assistant_text)
			agent->cb_on_assistant_text(content, agent->cb_user);
	}

	agent->state = CLM_STATE_COMPLETE;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	agent_turn_done(agent, 0);
}

/* Build a json tool_calls array from streamed accumulators, or NULL. */
static cJSON *
stream_build_tool_calls(struct clm_async_turn *turn)
{
	json_cleanup cJSON *arr = NULL;
	cJSON *ret;
	size_t i;

	if (turn->ncalls == 0)
		return NULL;
	arr = cJSON_CreateArray();
	if (arr == NULL)
		return NULL;

	for (i = 0; i < turn->ncalls; i++) {
		cJSON *call, *func;
		if (turn->calls[i].name == NULL)
			continue;
		call = cJSON_CreateObject();
		if (call == NULL)
			return NULL;
		cJSON_AddItemToArray(arr, call);
		cJSON_AddItemToObject(call, "id",
		    cJSON_CreateString(turn->calls[i].id ? turn->calls[i].id : ""));
		cJSON_AddItemToObject(call, "type", cJSON_CreateString("function"));
		func = cJSON_CreateObject();
		if (func == NULL)
			return NULL;
		cJSON_AddItemToObject(call, "function", func);
		cJSON_AddItemToObject(func, "name", cJSON_CreateString(turn->calls[i].name));
		cJSON_AddItemToObject(func, "arguments",
		    cJSON_CreateString(turn->calls[i].args ? turn->calls[i].args : "{}"));
	}

	if (cJSON_GetArraySize(arr) == 0)
		return NULL;
	ret = arr;
	arr = NULL;
	return ret;
}

static void
stream_finalize(struct clm_async_turn *turn)
{
	struct clm_agent *agent = turn->agent;
	json_cleanup cJSON *tool_calls = stream_build_tool_calls(turn);

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
	cJSON *message, *content = NULL, *tool_calls = NULL;
	int status = resp ? resp->status_code : -1;

	agent->inflight = NULL; /* request has completed */
	clm_debug("status=%d streaming=%d", status, turn->streaming);

	if (status != 200) {
		char buf[256];
		const char *detail = NULL;
		json_cleanup cJSON *errjson = NULL;

		/* Most OpenAI-compatible backends put the actually useful
		 * text in {"error":{"message":...}} on a non-2xx response
		 * (e.g. "model not found: X") -- surface that instead of a
		 * bare status code, which tells you nothing actionable.
		 * Falls back to a raw body snippet if it's not that shape
		 * (or not JSON at all), and to the bare code if the body is
		 * empty or unparseable. */
		if (resp != NULL && resp->body != NULL) {
			errjson = cJSON_Parse(resp->body);
			if (errjson != NULL) {
				cJSON *err = cJSON_GetObjectItemCaseSensitive(errjson, "error");
				cJSON *msg = cJSON_IsObject(err)
				    ? cJSON_GetObjectItemCaseSensitive(err, "message")
				    : NULL;
				if (cJSON_IsString(msg) && msg->valuestring != NULL)
					detail = msg->valuestring;
			}
		}

		if (detail != NULL)
			(void)snprintf(buf, sizeof(buf), "HTTP %d: %s", status, detail);
		else if (resp != NULL && resp->body != NULL && resp->body[0] != '\0')
			(void)snprintf(buf, sizeof(buf), "HTTP %d: %s", status, resp->body);
		else
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

	turn->parsed = resp && resp->body ? cJSON_Parse(resp->body) : NULL;
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
		cJSON *choices, *choice0, *jfinish = NULL, *jreason = NULL;
		struct clm_usage usage;
		choices = cJSON_GetObjectItemCaseSensitive(turn->parsed, "choices");
		if (choices != NULL &&
		    (choice0 = cJSON_GetArrayItem(choices, 0)) != NULL &&
		    (jfinish = cJSON_GetObjectItemCaseSensitive(choice0, "finish_reason")) != NULL &&
		    cJSON_IsString(jfinish))
			jreason = jfinish;
		emit_finish(agent, jreason ? cJSON_GetStringValue(jreason) : NULL);
		if (extract_usage(turn->parsed, &usage))
			emit_usage(agent, &usage);
	}

	/* Non-streaming reasoning, if the model exposes a think channel. */
	{
		cJSON *jreason = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
		if (jreason == NULL)
			jreason = cJSON_GetObjectItemCaseSensitive(message, "reasoning");
		if (jreason != NULL && cJSON_IsString(jreason) && agent->cb_on_reasoning)
			agent->cb_on_reasoning(cJSON_GetStringValue(jreason), agent->cb_user);
	}

	content = cJSON_GetObjectItemCaseSensitive(message, "content");
	tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
	agent_finish(agent, tool_calls,
	    content ? cJSON_GetStringValue(content) : NULL, false);
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
stream_merge_tool_calls(struct clm_async_turn *turn, cJSON *deltas)
{
	size_t i, n = (size_t)cJSON_GetArraySize(deltas);

	for (i = 0; i < n; i++) {
		cJSON *d = cJSON_GetArrayItem(deltas, i);
		cJSON *jidx, *jid, *func, *jname, *jargs;
		struct stream_call *call;
		int index = (int)i;

		if (d == NULL)
			continue;
		if ((jidx = cJSON_GetObjectItemCaseSensitive(d, "index")) != NULL)
			index = (int)cJSON_GetNumberValue(jidx);
		if (index < 0)
			continue;
		call = stream_get_call(turn, (size_t)index);
		if (call == NULL)
			continue;

		jid = cJSON_GetObjectItemCaseSensitive(d, "id");
		if (jid != NULL && call->id == NULL)
			call->id = strdup(cJSON_GetStringValue(jid));
		func = cJSON_GetObjectItemCaseSensitive(d, "function");
		if (func != NULL) {
			jname = cJSON_GetObjectItemCaseSensitive(func, "name");
			if (jname != NULL && call->name == NULL)
				call->name = strdup(cJSON_GetStringValue(jname));
			jargs = cJSON_GetObjectItemCaseSensitive(func, "arguments");
			if (jargs != NULL) {
				const char *frag = cJSON_GetStringValue(jargs);
				if (frag != NULL)
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
	json_cleanup cJSON *obj = NULL;
	cJSON *choices, *choice, *delta, *content = NULL, *tcs = NULL;
	const char *line = turn->line ? turn->line : "";
	const char *payload;

	/*
	 * Drop buffered/in-transit stream data only if the turn was cancelled,
	 * so a cancelled reply stops rendering at once. Do NOT gate on inflight:
	 * a normal completion also nulls inflight, and gating on it would drop
	 * the final content of a fast response whose body and completion land
	 * close together.
	 */
	if (agent->cancelling)
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

	obj = cJSON_Parse(payload);
	if (obj == NULL)
		return;

	/* Usage/timings arrive in a trailing chunk with an empty choices array
	 * (when stream_options.include_usage is set), so check it first. */
	if (!turn->have_usage && extract_usage(obj, &turn->usage))
		turn->have_usage = true;

	choices = cJSON_GetObjectItemCaseSensitive(obj, "choices");
	if (choices == NULL || !cJSON_IsArray(choices))
		return;
	choice = cJSON_GetArrayItem(choices, 0);
	if (choice == NULL)
		return;

	{
		cJSON *jfinish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
		if (jfinish != NULL && cJSON_IsString(jfinish)) {
			free(turn->finish_reason);
			turn->finish_reason = strdup(cJSON_GetStringValue(jfinish));
		}
	}

	delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
	if (delta == NULL)
		return;

	content = cJSON_GetObjectItemCaseSensitive(delta, "content");
	if (content != NULL && cJSON_IsString(content)) {
		const char *text = cJSON_GetStringValue(content);
		size_t tlen = text ? strlen(text) : 0;
		if (tlen > 0) {
			(void)sb_append(&turn->content, &turn->content_len,
			    &turn->content_cap, text, tlen);
			if (agent->cb_on_assistant_text)
				agent->cb_on_assistant_text(text, agent->cb_user);
		}
	}

	/* Reasoning / think channel, for models that emit one. */
	{
		cJSON *jr = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
		if (jr == NULL)
			jr = cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
		if (jr != NULL && cJSON_IsString(jr)) {
			const char *rt = cJSON_GetStringValue(jr);
			if (rt != NULL && rt[0] != '\0' && agent->cb_on_reasoning)
				agent->cb_on_reasoning(rt, agent->cb_user);
		}
	}

	tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
	if (tcs != NULL && cJSON_IsArray(tcs))
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
	agent_turn_done(agent, error_code);
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
	(void)agent_http_post(agent, agent->props_url, NULL, props_success_cb,
	    props_error_cb, NULL, agent, NULL);
}

/* Health probe completed (GET /v1/models): 2xx is online, anything else is
 * offline. user is the agent (not a turn). */
static void
health_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	int status = resp ? resp->status_code : -1;

	if (agent->cb_on_connection) {
		if (status >= 200 && status < 500) {
			/* 2xx = healthy; 4xx = server is reachable but the
			 * models endpoint is missing or auth-gated. Either
			 * way, the server is up. */
			agent->cb_on_connection(CLM_CONN_ONLINE, NULL,
			    agent->cb_user);
			if (agent->ctx_max == 0 && status >= 200 && status < 300)
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
	return agent_http_post(agent, agent->models_url, NULL, health_success_cb,
	    health_error_cb, NULL, agent, NULL);
}

struct models_list_ctx {
	void (*on_models)(char **ids, void *user);
	void (*on_error)(const char *msg, void *user);
	void *user;
};

static void
models_list_success_cb(struct clm_http_response *resp, void *user)
{
	struct models_list_ctx *ctx = user;
	char **ids = NULL;

	if (resp != NULL && resp->status_code >= 200 && resp->status_code < 300 &&
	    resp->body != NULL)
		ids = clm_parse_models_list(resp->body);

	if (ids != NULL) {
		if (ctx->on_models)
			ctx->on_models(ids, ctx->user);
		clm_free_models_list(ids);
	} else if (ctx->on_error) {
		char detail[80];
		if (resp == NULL) {
			ctx->on_error("request failed", ctx->user);
		} else if (resp->status_code < 200 || resp->status_code >= 300) {
			(void)snprintf(detail, sizeof(detail),
			    "HTTP %d", resp->status_code);
			ctx->on_error(detail, ctx->user);
		} else {
			ctx->on_error("unexpected response shape "
			    "(not {\"data\":[{\"id\":...}]})", ctx->user);
		}
	}
	if (resp)
		clm_http_response_free(resp);
	free(ctx);
}

static void
models_list_error_cb(int error_code, const char *error_msg, void *user)
{
	struct models_list_ctx *ctx = user;
	(void)error_code;
	if (ctx->on_error)
		ctx->on_error(error_msg ? error_msg : "request failed", ctx->user);
	free(ctx);
}

int
clm_agent_list_models(struct clm_agent *agent,
    void (*on_models)(char **ids, void *user),
    void (*on_error)(const char *msg, void *user),
    void *user)
{
	struct models_list_ctx *ctx;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(agent->models_url != NULL, -EINVAL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return -ENOMEM;
	ctx->on_models = on_models;
	ctx->on_error = on_error;
	ctx->user = user;

	/* NULL body => GET. ctx (not agent) is user here since this probe's
	 * callbacks need their own on_models/on_error/user, distinct from
	 * the agent-wide connection callbacks health_success_cb uses;
	 * out_req is NULL so this probe is not tracked for cancellation,
	 * same as clm_agent_check_connection. */
	if (agent_http_post(agent, agent->models_url, NULL,
	    models_list_success_cb, models_list_error_cb, NULL, ctx, NULL) != 0) {
		free(ctx);
		return -EIO;
	}
	return 0;
}

const char *
clm_agent_get_base_url(struct clm_agent *agent)
{
	return agent != NULL && agent->llm != NULL ? agent->llm->base_url : NULL;
}

const char *
clm_agent_get_api_key(struct clm_agent *agent)
{
	return agent != NULL && agent->llm != NULL ? agent->llm->api_key : NULL;
}

enum clm_provider
clm_agent_get_provider(struct clm_agent *agent)
{
	return agent != NULL && agent->llm != NULL ? agent->llm->provider
	                                            : CLM_PROVIDER_OPENAI;
}

int
clm_agent_cancel(struct clm_agent *agent)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);

	if (agent->inflight != NULL) {
		/* Waiting on the model: abort the request. Its error callback
		 * fires on_turn_done(-ECANCELED) and clears inflight. Mark the
		 * cancel so any buffered stream data is dropped rather than
		 * rendered (a normal completion also nulls inflight, so the flag
		 * is what distinguishes the two). */
		struct clm_http_call *req = agent->inflight;
		agent->inflight = NULL;
		agent->cancelling = true;
		agent->host->http_cancel(req);
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

int
clm_agent_set_provider(struct clm_agent *agent, const struct clm_cfg *cfg)
{
	char *new_url, *new_key, *new_model, *new_models_url, *new_props_url;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(cfg != NULL, -EINVAL);
	ASSERT_RETURN(cfg->base_url != NULL, -EINVAL);

	/* Refuse only while a turn is actually in flight. IDLE, COMPLETE, and
	 * ERROR all mean nothing is running right now -- in particular, a
	 * failed turn leaves the agent in CLM_STATE_ERROR permanently (there
	 * is no path back to IDLE without a new turn), so gating on == IDLE
	 * here would permanently lock out /model, /provider, and /agent
	 * switching after any single error: exactly the situation you want
	 * an escape hatch for (e.g. switch away from a model that just 403'd
	 * instead of being stuck repeating it). */
	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL ||
	    agent->state == CLM_STATE_RATE_LIMITED)
		return -EBUSY;

	new_url = strdup(cfg->base_url);
	new_key = (cfg->api_key != NULL && cfg->api_key[0] != '\0')
	    ? strdup(cfg->api_key) : strdup("sk-no-key-required");
	new_model = strdup(cfg->model != NULL ? cfg->model : "local-model");
	if (new_url == NULL || new_key == NULL || new_model == NULL) {
		free(new_url);
		free(new_key);
		free(new_model);
		return -ENOMEM;
	}

	/* Derive health/props URLs from the new base. */
	new_models_url = clm_derive_models_url(cfg->base_url);
	new_props_url = clm_derive_props_url(cfg->base_url);

	/* Swap. */
	free(agent->llm->base_url);
	free(agent->llm->api_key);
	free(agent->llm->model);
	agent->llm->base_url = new_url;
	agent->llm->api_key = new_key;
	agent->llm->model = new_model;
	agent->llm->provider = cfg->provider;

	free(agent->models_url);
	free(agent->props_url);
	agent->models_url = new_models_url;
	agent->props_url = new_props_url;

	/* Reset context info: a new server/model may have different limits,
	 * unless the new model/provider supplies an explicit override. */
	agent->ctx_max = cfg->context_size > 0 ? cfg->context_size : 0;
	agent->autocompact_pct = cfg->autocompact_pct > 0 ? cfg->autocompact_pct : 0;

	/* Only rebuild the LLM rate limiter if the new provider actually
	 * overrides it; otherwise keep the bucket (and its accumulated
	 * state) as-is rather than resetting it to the defaults. */
	if (cfg->rate_tokens_per_sec > 0 || cfg->rate_burst > 0) {
		int64_t rps = cfg->rate_tokens_per_sec > 0 ? cfg->rate_tokens_per_sec : 2000;
		int64_t burst = cfg->rate_burst > 0 ? cfg->rate_burst : 30000;
		struct clm_ratelimit *new_rl;

		if (clm_ratelimit_new(&new_rl, (size_t)rps, (size_t)burst) == 0) {
			clm_ratelimit_free(agent->llm_rl);
			agent->llm_rl = new_rl;
		}
	}

	clm_debug("provider switched: %s model=%s", cfg->base_url, new_model);
	return 0;
}

/*
 * Rate-limit timer for llm_dispatch: fires once enough tokens have
 * refilled in agent->llm_rl, then actually posts the turn that was
 * parked waiting for it.
 */
static void
on_llm_rl_timer(void *arg)
{
	struct clm_async_turn *turn = arg;
	struct clm_agent *agent = turn->agent;

	if (agent->llm_rl_timer != NULL) {
		agent->host->timer_cancel(agent->llm_rl_timer);
		agent->llm_rl_timer = NULL;
	}

	size_t est_tokens = strlen(turn->body) / 4;
	if (est_tokens == 0)
		est_tokens = 1;
	clm_ratelimit_consume(agent->llm_rl, est_tokens);
	agent_http_post(agent, agent->llm->base_url, turn->body,
			    clm_http_success_cb_wrapper, clm_http_error_cb_wrapper,
			    turn->streaming ? clm_http_data_cb_wrapper : NULL,
			    turn, &agent->inflight);
}

/*
 * Post an already-built turn's request, or park it behind a timer if
 * agent->llm_rl says we're going too fast. See llm_rl's comment in
 * internal.h: without this, a single logical turn that chains several
 * tool-calling round-trips can fire LLM requests back-to-back fast enough
 * to blow through a hosted backend's requests-per-minute limit even
 * though nothing else in clm paces LLM calls specifically (only tool
 * dispatch is rate-limited). Added after a real 429 against OpenAI caused
 * by exactly this pattern.
 */
static void
llm_dispatch(struct clm_agent *agent, struct clm_async_turn *turn)
{
	/* Estimate token cost from body size (1 token ~ 4 bytes) */
	size_t est_tokens = strlen(turn->body) / 4;
	if (est_tokens == 0)
		est_tokens = 1;

	if (agent->llm_rl == NULL || agent->host->timer_set == NULL ||
	    clm_ratelimit_allow(agent->llm_rl, est_tokens)) {
		/* Unlimited, no timer available to defer with, or allowed:
		 * dispatch now (clm_ratelimit_allow already consumed the
		 * token in the allowed case). */
		agent_http_post(agent, agent->llm->base_url, turn->body,
				    clm_http_success_cb_wrapper, clm_http_error_cb_wrapper,
				    turn->streaming ? clm_http_data_cb_wrapper : NULL,
				    turn, &agent->inflight);
		return;
	}

	{
		uint64_t delay_us = clm_ratelimit_delay(agent->llm_rl, est_tokens);
		uint64_t delay_ms = delay_us / 1000;
		if (delay_ms == 0)
			delay_ms = 1;
		agent->state = CLM_STATE_RATE_LIMITED;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent->host->timer_set(agent->host->ctx, delay_ms,
		    on_llm_rl_timer, turn, &agent->llm_rl_timer);
	}
}

static void
clm_agent_start_turn(struct clm_agent *agent)
{
	json_cleanup cJSON *req = NULL;
	cJSON *messages = NULL;
	cJSON *tools = NULL;
	cJSON *jmodel = NULL;
	cJSON *jstream = NULL;
	struct clm_async_turn *turn;
	autofree char *body_str = NULL;
	char *body;

	messages = clm_history_to_json(&agent->history, agent->compressor);
	tools = clm_tools_build_schema(agent);
	if (messages == NULL || tools == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	req = cJSON_CreateObject();
	if (req == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	jmodel = cJSON_CreateString(agent->llm->model);
	if (jmodel == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}
	cJSON_AddItemToObject(req, "model", jmodel);

	cJSON_AddItemToObject(req, "messages", messages);

	jstream = cJSON_CreateBool(agent->stream ? 1 : 0);
	if (jstream == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}
	cJSON_AddItemToObject(req, "stream", jstream);

	/* Ask the server to include token usage in the final stream chunk. */
	if (agent->stream) {
		cJSON *so = cJSON_CreateObject();
		if (so != NULL) {
			cJSON *inc = cJSON_CreateBool(1);
			if (inc != NULL)
				cJSON_AddItemToObject(so, "include_usage", inc);
			cJSON_AddItemToObject(req, "stream_options", so);
		}
	}

	cJSON_AddItemToObject(req, "tools", tools);

	/* Disable parallel tool calls -- a tool host that can only process
	 * one action at a time (e.g. a game bridge advancing one action per
	 * game turn) deadlocks on parallel dispatch, and serial calls keep
	 * tool ordering deterministic everywhere else. */
	cJSON_AddItemToObject(req, "parallel_tool_calls",
	    cJSON_CreateBool(0));
	body_str = cJSON_PrintUnformatted(req);
	if (body_str == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	body = strdup(body_str);
	if (body == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	turn = calloc(1, sizeof(struct clm_async_turn));
	if (turn == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	turn->agent = agent;
	turn->body = body;
	turn->streaming = agent->stream;
	body = NULL;

	clm_debug("posting body: %s", turn->body);

	llm_dispatch(agent, turn);
}

/*
 * Reach into a parsed completion response and return choices[0].message,
 * or NULL. The returned object is borrowed from parsed.
 */
static cJSON *
response_message(cJSON *parsed)
{
	cJSON *choices, *choice0, *message;

	choices = cJSON_GetObjectItemCaseSensitive(parsed, "choices");
	if (choices == NULL || !cJSON_IsArray(choices))
		return NULL;
	choice0 = cJSON_GetArrayItem(choices, 0);
	if (choice0 == NULL)
		return NULL;
	message = cJSON_GetObjectItemCaseSensitive(choice0, "message");
	if (message == NULL)
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
		agent_turn_done(agent, -ECANCELED);
		return;
	}

	if (status < 0) {
		clm_agent_set_error(agent, "tool execution failed");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, status);
		return;
	}

	if (agent->max_iterations > 0 &&
	    ++agent->iteration >= agent->max_iterations) {
		clm_agent_set_error(agent, "max iterations reached");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -E2BIG);
		return;
	}

	/*
	 * Mid-chain autocompact: a single turn can chain many tool-calling
	 * round-trips (tool result -> next LLM call -> more tool calls ->
	 * ...) before the conversational turn actually ends, so checking the
	 * threshold only once at full-turn completion (as tui.c's own
	 * end-of-turn check does) can let usage overshoot badly during a
	 * long chain. This is the safe place to catch that early: a tool
	 * batch just finished and the next LLM call hasn't started yet, so
	 * nothing is in flight for clm_agent_compact() to conflict with.
	 *
	 * clm_agent_compact() itself refuses to run while state is THINKING
	 * or CALLING_TOOL (see its own busy check) -- land on COMPLETE first,
	 * same as the cancel path above does, so it's willing to proceed.
	 * compact_success_cb/compact_error_cb check compact_resume_chain and,
	 * because it's set here, will call clm_agent_start_turn() directly
	 * on completion instead of firing cb_on_turn_done -- this is NOT a
	 * real turn ending, just a pause to shrink history, so a --forever
	 * caller must not see it as one (it would otherwise submit a fresh
	 * prompt on top of an unfinished tool chain).
	 */
	if (clm_agent_over_autocompact_threshold(agent)) {
		agent->state = CLM_STATE_COMPLETE;
		agent->compact_resume_chain = true;
		if (clm_agent_compact(agent) == 0) {
			agent->mid_chain_compact_started = true;
			agent->ctx_used = 0; /* stale pre-compaction reading;
			                      * see emit_usage() for when a
			                      * real one replaces it */
			if (agent->cb_on_state)
				agent->cb_on_state(agent->state, agent->cb_user);
			return;
		}
		/* Compact declined to start (shouldn't happen: state was
		 * just set to COMPLETE) or hit an immediate local failure --
		 * clear the flag and fall through to continue the chain
		 * without having shrunk anything, same as tui.c's own
		 * "not fatal, just try again next turn" handling. */
		agent->compact_resume_chain = false;
	}

	agent->state = CLM_STATE_THINKING;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	clm_agent_start_turn(agent);
}
