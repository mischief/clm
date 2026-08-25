// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "clm/agent.h"
#include "clm/http.h"
#include "clm/host.h"
#include "clm/llm.h"
#include "clm/tools.h"
#include "clm/history.h"
#include "clm/internal.h"
#include "clm/cleanup.h"
#include "clm/log.h"
#include "clm/provider.h"
#include "useful.h"
#include "banned.h"

static const char *default_system_prompt =
    "You are a helpful assistant. Answer from your own knowledge whenever you "
    "can. "
    "Use the provided tools only when the task requires reading or modifying "
    "the "
    "user's files or running a command on their system. When you already have "
    "the "
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
    "\n\nYou may periodically receive a line beginning with \"[context "
    "update]\" "
    "carrying the current date and time. Treat it as silent ambient context, "
    "not "
    "as a message from the user. Never announce, repeat, or comment on the "
    "time "
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
 * Build the session-start system prompt: the base prompt, a current-time
 * stamp, the note explaining future time updates, and the caller's host-facts
 * suffix. Returns a malloc'd string the caller must free, or NULL on OOM.
 */
static char *
build_system_prompt(const char *base, const char *suffix)
{
	char stamp[64];
	autofree char *out = NULL;
	size_t len;

	fmt_rfc2822(stamp, sizeof(stamp));
	if (suffix == NULL)
		suffix = "";

	len = strlen(base) + strlen(stamp) + strlen(time_context_note) +
	    strlen(suffix) + 24;
	out = malloc(len);
	if (out == NULL)
		return NULL;
	snprintf(out, len, "%s\n\ncurrent time: %s%s%s%s", base, stamp,
	    time_context_note, suffix[0] != '\0' ? "\n\n" : "", suffix);

	char *ret = out;
	out = NULL;
	return ret;
}

/*
 * Forget the server-side chain. Called whenever the history stops being an
 * append-only extension of what the server holds: the stored copy cannot be
 * edited, so a rewrite means the next request has to carry everything again.
 */
void
clm_agent_chain_reset(struct clm_agent *agent)
{
	if (agent == NULL)
		return;
	/* llm->prev_response_id borrows this, so drop the borrow first: the
	 * next request must not read freed memory. */
	if (agent->llm != NULL)
		agent->llm->prev_response_id = NULL;
	free(agent->resp_chain_id);
	agent->resp_chain_id = NULL;
	agent->resp_chain_sent = 0;
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
    clm_http_success_cb success, clm_http_error_cb error, clm_http_data_cb data,
    void *user, struct clm_http_call **out)
{
	const struct clm_provider_ops *ops =
	    clm_provider_ops_get(agent->llm->provider);
	autofreev char **auth_headers = NULL;
	struct clm_http_req req = {
	    .url = url,
	    .api_key = agent->llm->api_key,
	    .body = body,
	    .headers = NULL,
	    .client_suffix = NULL,
	};

	/* A provider with its own auth scheme (e.g. Anthropic's
	 * x-api-key/anthropic-version instead of a bearer token) supplies its
	 * own header set here and the default bearer header is suppressed --
	 * see clm/provider.h. */
	if (ops->build_auth_headers != NULL) {
		auth_headers = ops->build_auth_headers(agent->llm);
		if (auth_headers != NULL) {
			req.api_key = NULL;
			req.headers = (const char *const *)auth_headers;
		}
	}

	return agent->host->http_post(
	    agent->host->ctx, &req, success, error, data, user, out);
}

int
clm_agent_new(const struct clm_cfg *cfg, struct clm_host *host,
    const struct clm_callbacks *cb, void *user, struct clm_agent **out)
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
		agent->cb_on_notice = cb->on_notice;
		agent->cb_on_message = cb->on_message;
	}
	agent->cb_user = user;

	agent->models_url = clm_derive_models_url(cfg->base_url);
	agent->props_url = clm_derive_props_url(cfg->base_url);

	r = clm_llm_new(&agent->llm, cfg->provider, cfg->api_key, cfg->base_url,
	    cfg->model ? cfg->model : "local-model",
	    cfg->disable_parallel_tool_calls);
	if (r < 0) {
		free(agent);
		return r;
	}

	if (cfg->system_prompt != NULL) {
		agent->system_prompt_base = strdup(cfg->system_prompt);
		if (agent->system_prompt_base == NULL) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}
	if (cfg->system_prompt_suffix != NULL) {
		agent->system_prompt_suffix = strdup(cfg->system_prompt_suffix);
		if (agent->system_prompt_suffix == NULL) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}
	{
		const char *base = agent->system_prompt_base
		    ? agent->system_prompt_base
		    : default_system_prompt;
		autofree char *sys =
		    build_system_prompt(base, agent->system_prompt_suffix);
		struct clm_message *m;

		if (sys == NULL ||
		    (m = clm_history_add_system(
		         &agent->history, sys, agent->compressor)) == NULL) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
		clm_agent_emit_message(agent, m);
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

	/* LLM request rate limiter, estimated-token bucket -- see llm_rl in
	 * internal.h for why this exists separately from tool_rl, and why
	 * the fallback rate is deliberately very high rather than
	 * "conservative". Overridable per provider via cfg. */
	{
		int64_t rps = cfg->rate_tokens_per_sec > 0
		    ? cfg->rate_tokens_per_sec
		    : CLM_DEFAULT_LLM_RL_TOKENS_PER_SEC;
		int64_t burst = cfg->rate_burst > 0 ? cfg->rate_burst
		                                    : CLM_DEFAULT_LLM_RL_BURST;
		if (clm_ratelimit_new(
		        &agent->llm_rl, (size_t)rps, (size_t)burst) < 0) {
			clm_agent_free(agent);
			return -ENOMEM;
		}
	}

	/* Apply provider overrides from config */
	if (cfg->context_size > 0)
		agent->ctx_max = cfg->context_size;
	if (cfg->autocompact_pct > 0)
		agent->autocompact_pct = cfg->autocompact_pct;
	agent->autocompact_tokens = cfg->autocompact_tokens;

	agent->volatile_tools = cfg->volatile_tools;

	*out = agent;
	return 0;
}

void
clm_agent_free(struct clm_agent *agent)
{
	if (agent == NULL)
		return;

	clm_tools_detach(agent);
	clm_llm_free(agent->llm);
	clm_history_free(&agent->history);
	free(agent->system_prompt_base);
	free(agent->system_prompt_suffix);
	free(agent->resp_chain_id);
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
	if (agent->compact_rl_timer != NULL && agent->host != NULL &&
	    agent->host->timer_cancel != NULL)
		agent->host->timer_cancel(agent->compact_rl_timer);
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
clm_agent_set_compressor(
    struct clm_agent *agent, const struct clm_compressor *cz)
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

/* Auto-compact threshold, as a percentage of the window once one is known
 * (from GET /props on llama.cpp, the model document on Anthropic, or a
 * context_size override). Without a window, CLM_AUTOCOMPACT_FALLBACK_TOKENS
 * below stands in. Half the window rather than most of it: the whole
 * history is re-read on every turn, so carrying less of it is what makes a
 * turn cheap. */
#define CLM_AUTOCOMPACT_PCT 50

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
	/* An absolute cap wins wherever it is set: a share of a million-token
	 * window says nothing about an account whose throughput limit binds
	 * first. */
	if (agent->autocompact_tokens > 0 &&
	    agent->ctx_used >= agent->autocompact_tokens)
		return true;
	if (agent->ctx_max <= 0)
		return agent->ctx_used >= CLM_AUTOCOMPACT_FALLBACK_TOKENS;
	int pct = agent->autocompact_pct > 0 ? agent->autocompact_pct
	                                     : CLM_AUTOCOMPACT_PCT;
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
	 * Refresh the model's sense of time on a new turn once enough has
	 * passed. The Qwen3 template forbids a non-leading system message, so
	 * this rides in a user-role message framed as ambient context; the
	 * system prompt's note tells the model to treat "[context update]"
	 * lines silently. It is appended near the newest turn (outside the
	 * cached prefix) so it does not invalidate the server's prompt cache
	 * for the earlier conversation.
	 */
	{
		time_t now = time(NULL);

		if (now - agent->last_time_stamp >= CLM_TIME_STAMP_INTERVAL) {
			char stamp[64];
			autofree char *msg = NULL;

			fmt_rfc2822(stamp, sizeof(stamp));
			msg = malloc(strlen(stamp) + 100);
			if (msg != NULL) {
				snprintf(msg, strlen(stamp) + 100,
				    "[context update] current time: %s\n"
				    "(automatic context, not user input; do "
				    "not acknowledge)",
				    stamp);
				clm_agent_emit_message(agent,
				    clm_history_add_user(&agent->history, msg,
				        agent->compressor));
			}
			agent->last_time_stamp = now;
		}
	}

	{
		struct clm_message *m = clm_history_add_user(
		    &agent->history, prompt, agent->compressor);
		if (m == NULL) {
			clm_agent_set_error(agent, "out of memory");
			agent->state = CLM_STATE_ERROR;
			return -ENOMEM;
		}
		clm_agent_emit_message(agent, m);
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
		} else {
			size_t jlen =
			    strlen(agent->pending_notify) + strlen(text) + 10;
			joined = malloc(jlen);
			if (joined == NULL)
				return -ENOMEM;
			snprintf(joined, jlen, "%s\n\n%s",
			    agent->pending_notify, text);
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
	cJSON *parsed; /* non-streaming: the whole response */
	char *body;
	bool streaming;

	/* SSE assembly state */
	char *line; /* partial line across chunks */
	size_t line_len, line_cap;
	char *content; /* assembled assistant text */
	size_t content_len, content_cap;
	struct stream_call *calls; /* assembled tool calls, by index */
	size_t ncalls;
	char *finish_reason; /* captured from the stream */
	struct clm_usage usage;
	bool have_usage;

	/* History length this request represents, and the byte size of the
	 * whole conversation it stands for -- the request body may be much
	 * smaller when the server already holds the earlier turns. */
	size_t history_msgs;
	size_t ctx_bytes;
	int rl_retries;  /* rate-limit waits already served for this turn */
	char *rl_advice; /* server's rate-limit wording, if a stream carried one
	                  */

	/* Opaque per-turn scratch space for the provider's
	 * normalize_stream_event -- e.g. the Anthropic ops use this to carry
	 * input-token usage from message_start to message_delta. Always
	 * flat/pointer-free (see clm/provider.h), so a plain free() here is
	 * enough. */
	void *provider_stream_state;
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
	free(turn->provider_stream_state);
	free(turn->rl_advice);
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

/* Remember the provider's response id, so the next request can continue
 * from it instead of resending the conversation. */
static void
chain_note_response(struct clm_agent *agent, cJSON *parsed, size_t msgs)
{
	cJSON *rid;

	if (agent == NULL || parsed == NULL)
		return;
	rid = cJSON_GetObjectItemCaseSensitive(parsed, "provider_response_id");
	if (!cJSON_IsString(rid) || rid->valuestring[0] == '\0')
		return;
	if (agent->llm != NULL)
		agent->llm->prev_response_id = NULL; /* borrows the old one */
	free(agent->resp_chain_id);
	agent->resp_chain_id = strdup(rid->valuestring);
	agent->resp_chain_sent = agent->resp_chain_id != NULL ? msgs : 0;
}

static void on_llm_rl_timer(void *arg);

/*
 * Longest a turn waits out rate limits before giving up, and how many times.
 * The wait never goes below the floor, which doubles per attempt, even when
 * the server names a shorter one: the server's figure is when your own
 * bucket refills, which is wrong whenever something else keeps draining it.
 */
#define CLM_RL_RETRY_MAX 8
#define CLM_RL_RETRY_CAP_MS 60000
#define CLM_RL_RETRY_FLOOR_MS 1000
#define CLM_RL_RETRY_DEFAULT_MS 5000

/*
 * How long the server asked us to wait, in milliseconds. The body carries it
 * in prose ("Please try again in 14.583s", or "in 500ms"); the headers that
 * would say it directly are not kept by the HTTP layer. Falls back to a
 * fixed pause when nothing can be read.
 */
static uint64_t
server_delay_ms(const char *body)
{
	const char *p;
	double v;
	char unit[4];

	if (body == NULL)
		return CLM_RL_RETRY_DEFAULT_MS;
	p = strstr(body, "try again in ");
	if (p == NULL)
		return CLM_RL_RETRY_DEFAULT_MS;
	p += strlen("try again in ");
	if (sscanf(p, "%lf%3[a-z]", &v, unit) != 2 || v <= 0)
		return CLM_RL_RETRY_DEFAULT_MS;
	if (strncmp(unit, "ms", 2) == 0)
		return (uint64_t)v;
	if (unit[0] == 's')
		return (uint64_t)(v * 1000.0);
	if (unit[0] == 'm')
		return (uint64_t)(v * 60000.0);
	return CLM_RL_RETRY_DEFAULT_MS;
}

CLM_API uint64_t
clm_rl_retry_delay_ms(const char *body, int attempt)
{
	uint64_t delay = server_delay_ms(body);
	uint64_t floor_ms;

	if (attempt < 0)
		attempt = 0;
	if (attempt > 20)
		attempt = 20;
	floor_ms = (uint64_t)CLM_RL_RETRY_FLOOR_MS << attempt;

	if (delay < floor_ms)
		delay = floor_ms;
	if (delay > CLM_RL_RETRY_CAP_MS)
		delay = CLM_RL_RETRY_CAP_MS;
	return delay;
}

/*
 * Drop everything a turn accumulated from one attempt, keeping only what is
 * needed to send it again: the request body, the history it stands for, and
 * the retry count.
 */
static void
turn_reset_response(struct clm_async_turn *turn)
{
	size_t i;

	if (turn->parsed != NULL) {
		cJSON_Delete(turn->parsed);
		turn->parsed = NULL;
	}
	free(turn->line);
	turn->line = NULL;
	turn->line_len = turn->line_cap = 0;
	free(turn->content);
	turn->content = NULL;
	turn->content_len = turn->content_cap = 0;
	free(turn->finish_reason);
	turn->finish_reason = NULL;
	for (i = 0; i < turn->ncalls; i++) {
		free(turn->calls[i].id);
		free(turn->calls[i].name);
		free(turn->calls[i].args);
	}
	free(turn->calls);
	turn->calls = NULL;
	turn->ncalls = 0;
	free(turn->provider_stream_state);
	turn->provider_stream_state = NULL;
	free(turn->rl_advice);
	turn->rl_advice = NULL;
	memset(&turn->usage, 0, sizeof(turn->usage));
	turn->have_usage = false;
}

/*
 * A rate limit does not always arrive as a 429: the Responses API also
 * reports one inside an otherwise successful response, so recognize it by
 * what the server said.
 */
static bool
is_rate_limit_message(const char *msg)
{
	return msg != NULL &&
	    (strcasestr(msg, "rate limit") != NULL ||
	        strcasestr(msg, "rate_limit") != NULL);
}

/*
 * Wait out a rate limit and send the same request again. False if the turn
 * has already waited as often as it may, or there is no timer to wait on;
 * the caller then reports the failure as it otherwise would.
 */
static bool
rl_retry(struct clm_async_turn *turn, const char *advice)
{
	struct clm_agent *agent = turn->agent;
	uint64_t delay;

	if (turn->rl_retries >= CLM_RL_RETRY_MAX ||
	    agent->host->timer_set == NULL)
		return false;

	delay = clm_rl_retry_delay_ms(advice, turn->rl_retries);
	turn->rl_retries++;
	clm_debug("rate limited, retry %d in %llu ms", turn->rl_retries,
	    (unsigned long long)delay);
	turn_reset_response(turn);
	agent->state = CLM_STATE_RATE_LIMITED;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	agent->rl_parked_turn = turn;
	agent->host->timer_set(agent->host->ctx, delay, on_llm_rl_timer, turn,
	    &agent->llm_rl_timer);
	return true;
}

/* Return error.message from a canonical response, or NULL. Providers put
 * the server's own words there when a response failed rather than stopped. */
static const char *
response_error_message(cJSON *parsed)
{
	cJSON *err, *msg;

	if (parsed == NULL)
		return NULL;
	err = cJSON_GetObjectItemCaseSensitive(parsed, "error");
	msg = cJSON_IsObject(err)
	    ? cJSON_GetObjectItemCaseSensitive(err, "message")
	    : NULL;
	return cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : NULL;
}

/* Return choices[0].finish_reason from a canonical completion response. */
static const char *
response_finish_reason(cJSON *parsed)
{
	cJSON *choices, *choice, *reason;

	if (parsed == NULL)
		return NULL;
	choices = cJSON_GetObjectItemCaseSensitive(parsed, "choices");
	choice = choices != NULL ? cJSON_GetArrayItem(choices, 0) : NULL;
	reason = choice != NULL
	    ? cJSON_GetObjectItemCaseSensitive(choice, "finish_reason")
	    : NULL;
	return cJSON_IsString(reason) ? cJSON_GetStringValue(reason) : NULL;
}

/*
 * What compaction keeps verbatim: the newest turns fitting this share of the
 * window, never fewer than CLM_COMPACT_KEEP_MIN. Every turn pays to re-read
 * whatever the history carries, so the share is deliberately small: a
 * smaller tail costs more frequent compaction and buys a cheaper turn.
 */
#define CLM_COMPACT_KEEP_PCT 10
#define CLM_COMPACT_KEEP_MIN 2
#define CLM_BYTES_PER_TOKEN 4

/* Instruction appended to drive the summarization call. */
static const char *compact_prompt =
    "Summarize the conversation so far into a compact briefing that lets you "
    "continue seamlessly. Preserve: decisions made, file paths touched, "
    "commands run and their outcomes, and any open tasks or unresolved "
    "problems. Be terse and factual. Output only the summary.";

/*
 * Extract choices[0].message.content from a parsed completion into a malloc'd
 * string, or NULL. Borrowed parse; caller frees the returned copy.
 *
 * Falls back to the reasoning channel (reasoning_content, or reasoning --
 * same two names checked elsewhere in this file for the streaming case) when
 * content is missing or empty. This matters for the compaction call
 * specifically: it re-sends the whole (near-max, since compaction only
 * triggers close to the context limit) history, leaving a "thinking" model
 * little completion budget left over, so it can burn the entire response on
 * reasoning and hit finish_reason "length" before ever emitting content.
 * A half-finished chain-of-thought is a worse summary than a real one, but
 * it is still strictly better than failing compaction outright -- which,
 * for a model that reasons this heavily, would mean compaction can never
 * succeed at all, right when it's needed most.
 */
static char *
extract_message_content(cJSON *parsed)
{
	cJSON *message, *content;

	message = response_message(parsed);
	if (message == NULL)
		return NULL;
	content = cJSON_GetObjectItemCaseSensitive(message, "content");
	if (content != NULL && cJSON_IsString(content) &&
	    cJSON_GetStringValue(content)[0] != '\0')
		return strdup(cJSON_GetStringValue(content));

	content =
	    cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
	if (content == NULL)
		content =
		    cJSON_GetObjectItemCaseSensitive(message, "reasoning");
	if (content == NULL || !cJSON_IsString(content) ||
	    cJSON_GetStringValue(content)[0] == '\0')
		return NULL;
	return strdup(cJSON_GetStringValue(content));
}

/*
 * Turn an HTTP error response into the useful diagnostic exposed by normal
 * completion requests.  Compaction is a separate one-shot request, but it
 * can fail for precisely the same provider-side reasons (especially context
 * limits), so do not hide the status/error envelope behind a generic error.
 */
static void
format_http_error(
    const struct clm_http_response *resp, int status, char *buf, size_t bufsz)
{
	const char *detail = NULL;
	json_cleanup cJSON *errjson = NULL;

	if (resp != NULL && resp->body != NULL && resp->body[0] != '\0') {
		errjson = cJSON_Parse(resp->body);
		if (errjson != NULL) {
			cJSON *err =
			    cJSON_GetObjectItemCaseSensitive(errjson, "error");
			cJSON *msg = cJSON_IsObject(err)
			    ? cJSON_GetObjectItemCaseSensitive(err, "message")
			    : NULL;
			if (cJSON_IsString(msg) && msg->valuestring != NULL)
				detail = msg->valuestring;
		}
	}

	if (detail != NULL)
		(void)snprintf(buf, bufsz, "HTTP %d: %s", status, detail);
	else if (resp != NULL && resp->body != NULL && resp->body[0] != '\0')
		(void)snprintf(buf, bufsz, "HTTP %d: %s", status, resp->body);
	else if (resp != NULL)
		(void)snprintf(
		    buf, bufsz, "HTTP %d: empty response body", status);
	else
		(void)snprintf(buf, bufsz, "HTTP %d", status);
}

/* Release the stashed compaction request body once no retry can need it. */
static void
compact_done(struct clm_agent *agent)
{
	free(agent->compact_body);
	agent->compact_body = NULL;
}

static void compact_post(struct clm_agent *agent);

/* Resend a compaction request that was turned away by a rate limit. */
static void
on_compact_rl_timer(void *arg)
{
	struct clm_agent *agent = arg;

	if (agent->compact_rl_timer != NULL) {
		agent->host->timer_cancel(agent->compact_rl_timer);
		agent->compact_rl_timer = NULL;
	}
	compact_post(agent);
}

/*
 * Wait out a rate limit and send the compaction request again. Compaction
 * carries the whole history, so it is the largest request a session makes and
 * the first one a limit turns away -- and giving up leaves the context just
 * as oversized, which asks for another compaction, which is refused in turn.
 */
static bool
compact_rl_retry(struct clm_agent *agent, const char *advice)
{
	uint64_t delay;

	if (agent->compact_body == NULL ||
	    agent->compact_rl_retries >= CLM_RL_RETRY_MAX ||
	    agent->host->timer_set == NULL)
		return false;

	delay = clm_rl_retry_delay_ms(advice, agent->compact_rl_retries);
	agent->compact_rl_retries++;
	clm_debug("compaction rate limited, retry %d in %llu ms",
	    agent->compact_rl_retries, (unsigned long long)delay);
	agent->state = CLM_STATE_RATE_LIMITED;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	agent->host->timer_set(agent->host->ctx, delay, on_compact_rl_timer,
	    agent, &agent->compact_rl_timer);
	return true;
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

	if (status == 429) {
		autofree char *advice = resp != NULL && resp->body != NULL
		    ? strdup(resp->body)
		    : NULL;

		agent->compact_resume_chain = resume;
		if (compact_rl_retry(agent, advice)) {
			if (resp != NULL)
				clm_http_response_free(resp);
			return;
		}
		agent->compact_resume_chain = false;
	}

	if (status != 200 || resp == NULL || resp->body == NULL) {
		char detail[256];

		format_http_error(resp, status, detail, sizeof(detail));
		if (resp)
			clm_http_response_free(resp);
		compact_done(agent);
		if (resume) {
			/* Mid-chain: not fatal, just didn't shrink anything --
			 * continue the interrupted chain as-is rather than
			 * landing the whole turn in an error state over a
			 * compaction hiccup. */
			clm_agent_set_error(agent, detail);
			agent->mid_chain_compact_failed = true;
			clm_agent_start_turn(agent);
			return;
		}
		agent_fail(agent, detail, -EIO);
		return;
	}
	parsed = cJSON_Parse(resp->body);
	clm_debug("compact response body: %s", resp->body);
	clm_http_response_free(resp);

	if (parsed != NULL) {
		const struct clm_provider_ops *ops =
		    clm_provider_ops_get(agent->llm->provider);

		if (ops->normalize_response != NULL)
			parsed = ops->normalize_response(parsed);
	}

	summary = parsed ? extract_message_content(parsed) : NULL;
	if (summary == NULL || summary[0] == '\0') {
		const char *emsg = response_error_message(parsed);
		const char *reason = response_finish_reason(parsed);

		if (is_rate_limit_message(emsg)) {
			agent->compact_resume_chain = resume;
			if (compact_rl_retry(agent, emsg))
				return;
			agent->compact_resume_chain = false;
		}
		const char *why =
		    reason != NULL && strcmp(reason, "content_filter") == 0
		    ? "compaction stopped by content filter"
		    : "compaction produced no summary";

		compact_done(agent);
		if (resume) {
			clm_agent_set_error(agent, why);
			agent->mid_chain_compact_failed = true;
			clm_agent_start_turn(agent);
			return;
		}
		agent_fail(agent, why, -EIO);
		return;
	}

	{
		/* No window discovered means no percentage to take, so the
		 * cost-cap fallback stands in for one. */
		int64_t window = agent->ctx_max > 0
		    ? agent->ctx_max
		    : CLM_AUTOCOMPACT_FALLBACK_TOKENS;
		size_t keep_bytes = (size_t)window * CLM_COMPACT_KEEP_PCT /
		    100 * CLM_BYTES_PER_TOKEN;
		int folded =
		    clm_history_compact_within(&agent->history, summary,
		        keep_bytes, CLM_COMPACT_KEEP_MIN, agent->compressor);

		/* The server's stored copy still holds what was folded. */
		if (folded > 0)
			clm_agent_chain_reset(agent);
		/* folded == 0 is failure here, not success: the history had no
		 * valid cut point, so nothing shrank and the summary we just
		 * paid a full-history LLM call for was discarded. Reporting it
		 * as success is what caused the "compact forever" loop -- the
		 * context stayed over threshold and every subsequent tool
		 * round-trip re-triggered another futile summarize call. */
		if (folded <= 0) {
			const char *why = folded < 0
			    ? "compaction failed"
			    : "compaction made no progress";
			compact_done(agent);
			if (resume) {
				clm_agent_set_error(agent, why);
				agent->mid_chain_compact_failed = true;
				clm_agent_start_turn(agent);
				return;
			}
			agent_fail(agent, why, folded < 0 ? folded : -EAGAIN);
			return;
		}
	}

	compact_done(agent);
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
	compact_done(agent);

	if (resume && error_code != -ECANCELED) {
		/* Mid-chain, non-cancel failure: same "not fatal, keep going"
		 * handling as the success path above -- don't land the whole
		 * turn in CLM_STATE_ERROR over a compaction hiccup. */
		clm_agent_set_error(
		    agent, error_msg ? error_msg : "compaction request failed");
		agent->mid_chain_compact_failed = true;
		clm_agent_start_turn(agent);
		return;
	}

	if (error_code == -ECANCELED)
		agent->state = CLM_STATE_COMPLETE;
	else {
		clm_agent_set_error(
		    agent, error_msg ? error_msg : "compaction request failed");
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
/* Send the stashed compaction body. Shared by the first attempt and by the
 * rate-limit retry, which must resend exactly the same request. */
static void
compact_post(struct clm_agent *agent)
{
	agent->state = CLM_STATE_THINKING;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	if (agent_http_post(agent, agent->llm->base_url, agent->compact_body,
	        compact_success_cb, compact_error_cb, NULL, agent,
	        &agent->inflight) < 0) {
		free(agent->compact_body);
		agent->compact_body = NULL;
		clm_agent_set_error(agent, "compaction request failed");
		compact_error_cb(-EIO, "compaction request failed", agent);
	}
}

int
clm_agent_compact(struct clm_agent *agent)
{
	const struct clm_provider_ops *ops;
	json_cleanup cJSON *req = NULL;
	cJSON *messages, *msg, *tools;
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
	if (messages == NULL)
		return -ENOMEM;

	/* Append the summarization instruction as a trailing user message. */
	msg = cJSON_CreateObject();
	if (msg == NULL) {
		cJSON_Delete(messages);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
	cJSON_AddItemToObject(
	    msg, "content", cJSON_CreateString(compact_prompt));
	cJSON_AddItemToArray(messages, msg);

	/* Build through the provider seam rather than hand-serializing the
	 * canonical chat-completions shape. Responses API providers require
	 * `input`, not `messages`; build_request() also owns messages, so do
	 * not delete it below.
	 *
	 * The tool schemas go out even though this call must not call a tool:
	 * they head the prefix the provider caches, so dropping them here
	 * makes every compaction a full-price prefill of the whole history.
	 * forbid_tool_calls() blocks the calls instead, from outside the
	 * cached prefix. */
	ops = clm_provider_ops_get(agent->llm->provider);
	tools = agent->tools_unsupported ? NULL : clm_tools_build_schema(agent);
	/* This request carries the conversation itself, so it continues
	 * nothing -- and the fold that follows invalidates the chain anyway. */
	agent->llm->prev_response_id = NULL;
	req = ops->build_request(agent->llm, messages, tools, false);
	if (req == NULL)
		return -ENOMEM;
	if (tools != NULL && ops->forbid_tool_calls != NULL)
		ops->forbid_tool_calls(req);

	body_str = cJSON_PrintUnformatted(req);
	if (body_str == NULL)
		return -ENOMEM;
	body = strdup(body_str);
	if (body == NULL)
		return -ENOMEM;

	/* curl borrows the POST body (CURLOPT_POSTFIELDS), so it must outlive
	 * the request; stash it and free it when the request completes. */
	free(agent->compact_body);
	agent->compact_body = body;
	agent->compact_rl_retries = 0;

	agent->state = CLM_STATE_THINKING;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	r = agent_http_post(agent, agent->llm->base_url, body,
	    compact_success_cb, compact_error_cb, NULL, agent,
	    &agent->inflight);
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
		agent->cb_on_finish_reason(
		    finish_from_str(reason), agent->cb_user);
}

/* Read usage/timings from a response object. Returns true if usage present. */
static bool
extract_usage(cJSON *root, struct clm_usage *out)
{
	cJSON *u, *t, *v, *d;

	u = cJSON_GetObjectItemCaseSensitive(root, "usage");
	if (u == NULL || !cJSON_IsObject(u))
		return false;
	memset(out, 0, sizeof(*out));
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens")) != NULL)
		out->prompt_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "completion_tokens")) !=
	    NULL)
		out->completion_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "total_tokens")) != NULL)
		out->total_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "cache_read_tokens")) !=
	    NULL)
		out->cache_read_tokens = (int)cJSON_GetNumberValue(v);
	if ((v = cJSON_GetObjectItemCaseSensitive(u, "cache_write_tokens")) !=
	    NULL)
		out->cache_write_tokens = (int)cJSON_GetNumberValue(v);
	/* OpenAI-compatible servers instead nest the cache hit under
	 * prompt_tokens_details, already counted inside prompt_tokens. */
	d = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens_details");
	if (cJSON_IsObject(d) &&
	    (v = cJSON_GetObjectItemCaseSensitive(d, "cached_tokens")) != NULL)
		out->cache_read_tokens = (int)cJSON_GetNumberValue(v);
	t = cJSON_GetObjectItemCaseSensitive(root, "timings");
	if (t != NULL && cJSON_IsObject(t) &&
	    (v = cJSON_GetObjectItemCaseSensitive(t, "predicted_per_second")) !=
	        NULL)
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
	agent->ctx_used =
	    (int64_t)usage->prompt_tokens + usage->completion_tokens;
	clm_debug("usage: prompt=%d (cache read=%d write=%d) completion=%d "
	          "ctx_used=%lld/%lld",
	    usage->prompt_tokens, usage->cache_read_tokens,
	    usage->cache_write_tokens, usage->completion_tokens,
	    (long long)agent->ctx_used, (long long)agent->ctx_max);
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
agent_finish(struct clm_agent *agent, cJSON *tool_calls, const char *content,
    const char *finish_reason, bool streamed)
{
	/* A content filter is not a successful empty answer.  The UI has
	 * already received the finish notification, but complete the turn as an
	 * error too so callers get an actionable diagnostic rather than only
	 * the transient "[stopped: content filter]" notice. */
	if (finish_reason != NULL &&
	    strcmp(finish_reason, "content_filter") == 0) {
		agent_fail(
		    agent, "response stopped by content filter", -EACCES);
		return;
	}

	/* The provider reported a failed response rather than a stopped one.
	 * agent->last_error already holds whatever the server said. */
	if (finish_reason != NULL && strcmp(finish_reason, "error") == 0) {
		const char *why = clm_agent_get_last_error(agent);

		agent_fail(agent,
		    why != NULL && why[0] != '\0' ? why
		                                  : "provider reported a "
		                                    "failed response",
		    -EIO);
		return;
	}

	if (tool_calls != NULL && cJSON_IsArray(tool_calls) &&
	    cJSON_GetArraySize(tool_calls) > 0) {
		int r;
		if (content != NULL && content[0] != '\0')
			clm_debug("[think] %.*s",
			    (int)(strlen(content) > 200 ? 200
			                                : strlen(content)),
			    content);
		agent->state = CLM_STATE_CALLING_TOOL;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		r = clm_tools_dispatch(agent, tool_calls);
		if (r < 0)
			agent_fail(agent, "failed to dispatch tools", r);
		return;
	}

	if (content != NULL) {
		struct clm_message *m;
		clm_debug("[think] %.*s",
		    (int)(strlen(content) > 200 ? 200 : strlen(content)),
		    content);
		m = clm_history_add_assistant_text(
		    &agent->history, content, agent->compressor);
		if (m == NULL) {
			agent_fail(agent, "out of memory", -ENOMEM);
			return;
		}
		clm_agent_emit_message(agent, m);
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
		    cJSON_CreateString(
		        turn->calls[i].id ? turn->calls[i].id : ""));
		cJSON_AddItemToObject(
		    call, "type", cJSON_CreateString("function"));
		func = cJSON_CreateObject();
		if (func == NULL)
			return NULL;
		cJSON_AddItemToObject(call, "function", func);
		cJSON_AddItemToObject(
		    func, "name", cJSON_CreateString(turn->calls[i].name));
		cJSON_AddItemToObject(func, "arguments",
		    cJSON_CreateString(
		        turn->calls[i].args ? turn->calls[i].args : "{}"));
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

	/*
	 * A stream that failed on a rate limit produced no text and no calls,
	 * so resending it repeats nothing the user has already seen.
	 */
	if (turn->rl_advice != NULL && turn->content_len == 0 &&
	    tool_calls == NULL && rl_retry(turn, turn->rl_advice))
		return;

	emit_finish(agent, turn->finish_reason);
	if (turn->have_usage)
		emit_usage(agent, &turn->usage);
	agent_finish(
	    agent, tool_calls, turn->content, turn->finish_reason, true);
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
				cJSON *err = cJSON_GetObjectItemCaseSensitive(
				    errjson, "error");
				cJSON *msg = cJSON_IsObject(err)
				    ? cJSON_GetObjectItemCaseSensitive(
				          err, "message")
				    : NULL;
				if (cJSON_IsString(msg) &&
				    msg->valuestring != NULL)
					detail = msg->valuestring;
			}
		}

		/* Ollama's signature for "this model has no tool-calling
		 * template": a plain-text 400 body, no JSON envelope, e.g.
		 * "registry.ollama.ai/foo/bar:8b does not support tools".
		 * Detected live against a real ollama server -- every turn
		 * we sent had "tools" attached (clm always registers at
		 * least the shell/bg builtins), so a model without tool
		 * support failed every single turn with no way to have a
		 * plain conversation with it at all. Retry once with no
		 * "tools" field and remember the model can't take it for
		 * the rest of this session (see tools_unsupported), instead
		 * of repeating the same failing request forever. */
		/*
		 * Rate limited: the server says how long to wait, so wait
		 * that long and send the same request again rather than
		 * ending the turn. The agent parks in the same state the
		 * token bucket uses, so the UI already shows it waiting.
		 */
		if (status == 429) {
			autofree char *advice =
			    (resp != NULL && resp->body != NULL)
			    ? strdup(resp->body)
			    : NULL;

			if (rl_retry(turn, advice)) {
				if (resp != NULL)
					clm_http_response_free(resp);
				return;
			}
		}

		/*
		 * The stored response is gone (expired, or another process
		 * ended it). Nothing is lost: clm still holds the whole
		 * conversation, so drop the chain and send it again.
		 */
		if (agent->resp_chain_id != NULL && resp != NULL &&
		    resp->body != NULL &&
		    strstr(resp->body, "previous_response") != NULL) {
			clm_agent_chain_reset(agent);
			clm_http_response_free(resp);
			clm_async_turn_free(turn);
			clm_agent_start_turn(agent);
			return;
		}

		if (status == 400 && !agent->tools_unsupported &&
		    resp != NULL && resp->body != NULL &&
		    strstr(resp->body, "does not support tools") != NULL) {
			agent->tools_unsupported = true;
			clm_http_response_free(resp);
			clm_async_turn_free(turn);
			if (agent->cb_on_notice)
				agent->cb_on_notice(
				    "model does not support tool calls; "
				    "continuing without tools",
				    agent->cb_user);
			clm_agent_start_turn(agent);
			return;
		}

		if (detail != NULL)
			(void)snprintf(
			    buf, sizeof(buf), "HTTP %d: %s", status, detail);
		else if (resp != NULL && resp->body != NULL &&
		    resp->body[0] != '\0')
			(void)snprintf(buf, sizeof(buf), "HTTP %d: %s", status,
			    resp->body);
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

	{
		const struct clm_provider_ops *ops =
		    clm_provider_ops_get(agent->llm->provider);
		if (ops->normalize_response != NULL) {
			cJSON *norm = ops->normalize_response(turn->parsed);
			turn->parsed = norm; /* normalize_response always
			                        consumes its input */
			if (turn->parsed == NULL) {
				clm_async_turn_free(turn);
				agent_fail(agent,
				    "could not parse llm response", -EIO);
				return;
			}
		}
	}

	message = response_message(turn->parsed);
	if (message == NULL) {
		clm_async_turn_free(turn);
		agent_fail(
		    agent, "could not extract message from llm response", -EIO);
		return;
	}

	{
		cJSON *choices, *choice0, *jfinish = NULL, *jreason = NULL;
		struct clm_usage usage;
		choices =
		    cJSON_GetObjectItemCaseSensitive(turn->parsed, "choices");
		if (choices != NULL &&
		    (choice0 = cJSON_GetArrayItem(choices, 0)) != NULL &&
		    (jfinish = cJSON_GetObjectItemCaseSensitive(
		         choice0, "finish_reason")) != NULL &&
		    cJSON_IsString(jfinish))
			jreason = jfinish;
		emit_finish(
		    agent, jreason ? cJSON_GetStringValue(jreason) : NULL);
		if (extract_usage(turn->parsed, &usage))
			emit_usage(agent, &usage);
	}

	/* Non-streaming reasoning, if the model exposes a think channel. */
	{
		cJSON *jreason = cJSON_GetObjectItemCaseSensitive(
		    message, "reasoning_content");
		if (jreason == NULL)
			jreason = cJSON_GetObjectItemCaseSensitive(
			    message, "reasoning");
		if (jreason != NULL && cJSON_IsString(jreason) &&
		    agent->cb_on_reasoning)
			agent->cb_on_reasoning(
			    cJSON_GetStringValue(jreason), agent->cb_user);
	}

	content = cJSON_GetObjectItemCaseSensitive(message, "content");
	tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
	{
		const char *emsg = response_error_message(turn->parsed);

		if (is_rate_limit_message(emsg) && rl_retry(turn, emsg))
			return;
		if (emsg != NULL)
			clm_agent_set_error(agent, emsg);
	}
	chain_note_response(agent, turn->parsed, turn->history_msgs);
	agent_finish(agent, tool_calls,
	    content ? cJSON_GetStringValue(content) : NULL,
	    response_finish_reason(turn->parsed), false);
	clm_async_turn_free(turn);
}

/* ------------------------------------------------------------------ */
/* SSE streaming parser                                                */
/* ------------------------------------------------------------------ */

static struct stream_call *
stream_get_call(struct clm_async_turn *turn, size_t index)
{
	if (index >= turn->ncalls) {
		struct stream_call *p =
		    realloc(turn->calls, (index + 1) * sizeof(*turn->calls));
		if (p == NULL)
			return NULL;
		turn->calls = p;
		while (turn->ncalls <= index)
			memset(&turn->calls[turn->ncalls++], 0,
			    sizeof(*turn->calls));
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
		if ((jidx = cJSON_GetObjectItemCaseSensitive(d, "index")) !=
		    NULL)
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
				call->name =
				    strdup(cJSON_GetStringValue(jname));
			jargs =
			    cJSON_GetObjectItemCaseSensitive(func, "arguments");
			if (jargs != NULL) {
				const char *frag = cJSON_GetStringValue(jargs);
				if (frag != NULL)
					(void)sb_append(&call->args,
					    &call->args_len, &call->args_cap,
					    frag, strlen(frag));
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
	 * so a cancelled reply stops rendering at once. Do NOT gate on
	 * inflight: a normal completion also nulls inflight, and gating on it
	 * would drop the final content of a fast response whose body and
	 * completion land close together.
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

	{
		const struct clm_provider_ops *ops =
		    clm_provider_ops_get(agent->llm->provider);
		if (ops->normalize_stream_event != NULL) {
			cJSON *norm = ops->normalize_stream_event(
			    obj, &turn->provider_stream_state);
			cJSON_Delete(obj);
			obj = norm; /* NULL means "nothing to merge from this
			               event" */
			if (obj == NULL)
				return;
		}
	}

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
		cJSON *jfinish =
		    cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
		const char *emsg;

		if (jfinish != NULL && cJSON_IsString(jfinish)) {
			free(turn->finish_reason);
			turn->finish_reason =
			    strdup(cJSON_GetStringValue(jfinish));
		}
		/* Keep what the server said about a failed response: the
		 * chunk carrying it is gone by the time the turn ends. */
		emsg = response_error_message(obj);
		if (emsg != NULL) {
			clm_agent_set_error(turn->agent, emsg);
			if (is_rate_limit_message(emsg)) {
				free(turn->rl_advice);
				turn->rl_advice = strdup(emsg);
			}
		}
		chain_note_response(turn->agent, obj, turn->history_msgs);
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
				agent->cb_on_assistant_text(
				    text, agent->cb_user);
		}
	}

	/* Reasoning / think channel, for models that emit one. */
	{
		cJSON *jr = cJSON_GetObjectItemCaseSensitive(
		    delta, "reasoning_content");
		if (jr == NULL)
			jr = cJSON_GetObjectItemCaseSensitive(
			    delta, "reasoning");
		if (jr != NULL && cJSON_IsString(jr)) {
			const char *rt = cJSON_GetStringValue(jr);
			if (rt != NULL && rt[0] != '\0' &&
			    agent->cb_on_reasoning)
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
	 * leave no error string, so the status bar does not show "error" and
	 * the next prompt just works.
	 */
	if (error_code == -ECANCELED) {
		agent->state = CLM_STATE_COMPLETE;
	} else {
		clm_agent_set_error(
		    agent, error_msg ? error_msg : "http request failed");
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

	if (resp != NULL && resp->status_code >= 200 &&
	    resp->status_code < 300 && resp->body != NULL &&
	    clm_parse_props(resp->body, &ctx) == 0) {
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
	(void)user; /* no /props (not llama.cpp, or old build): leave ctx
	               unknown */
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

/* GET <models_url>/<model> completed: take the window the backend reports. */
static void
model_meta_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	int64_t ctx = 0;

	if (resp != NULL && resp->status_code >= 200 &&
	    resp->status_code < 300 && resp->body != NULL &&
	    clm_parse_model_ctx(resp->body, &ctx) == 0)
		agent->ctx_max = ctx;
	if (resp)
		clm_http_response_free(resp);
}

/*
 * Ask the backend about the model itself: Anthropic has no /props, but its
 * GET /v1/models/<id> carries max_input_tokens. Without a window the gauge
 * reads nothing and compaction falls back to a fixed token count.
 */
static void
clm_agent_fetch_model_meta(struct clm_agent *agent)
{
	autofree char *url = NULL;

	if (agent->models_url == NULL || agent->llm == NULL ||
	    agent->llm->model == NULL)
		return;
	if (asprintf(&url, "%s/%s", agent->models_url, agent->llm->model) < 0)
		return;
	(void)agent_http_post(agent, url, NULL, model_meta_success_cb,
	    props_error_cb, NULL, agent, NULL);
}

/* Learn the context window however this backend exposes it. */
static void
clm_agent_fetch_ctx_max(struct clm_agent *agent)
{
	if (agent->llm != NULL &&
	    agent->llm->provider == CLM_PROVIDER_ANTHROPIC)
		clm_agent_fetch_model_meta(agent);
	else
		clm_agent_fetch_props(agent);
}

/* Health probe completed (GET /v1/models): 2xx is online, anything else is
 * offline. user is the agent (not a turn). */
static void
health_success_cb(struct clm_http_response *resp, void *user)
{
	struct clm_agent *agent = user;
	int status = resp ? resp->status_code : -1;

	/* 2xx = healthy; 4xx = server is reachable but the models endpoint is
	 * missing or auth-gated. Either way, the server is up. */
	if (agent->cb_on_connection) {
		if (status >= 200 && status < 500) {
			agent->cb_on_connection(
			    CLM_CONN_ONLINE, NULL, agent->cb_user);
		} else {
			char detail[64];
			(void)snprintf(
			    detail, sizeof(detail), "HTTP %d", status);
			agent->cb_on_connection(
			    CLM_CONN_OFFLINE, detail, agent->cb_user);
		}
	}
	/* The window drives compaction, not just the gauge, so learn it even
	 * when no UI is listening for connection events. */
	if (agent->ctx_max == 0 && status >= 200 && status < 300)
		clm_agent_fetch_ctx_max(agent);
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
		agent->cb_on_connection(
		    CLM_CONN_CHECKING, NULL, agent->cb_user);

	/* NULL body => GET. user is the agent, distinct from turn requests;
	 * out_req is NULL so this probe is not tracked for cancellation. */
	return agent_http_post(agent, agent->models_url, NULL,
	    health_success_cb, health_error_cb, NULL, agent, NULL);
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

	if (resp != NULL && resp->status_code >= 200 &&
	    resp->status_code < 300 && resp->body != NULL)
		ids = clm_parse_models_list(resp->body);

	if (ids != NULL) {
		if (ctx->on_models)
			ctx->on_models(ids, ctx->user);
		clm_free_models_list(ids);
	} else if (ctx->on_error) {
		char detail[80];
		if (resp == NULL) {
			ctx->on_error("request failed", ctx->user);
		} else if (resp->status_code < 200 ||
		    resp->status_code >= 300) {
			(void)snprintf(detail, sizeof(detail), "HTTP %d",
			    resp->status_code);
			ctx->on_error(detail, ctx->user);
		} else {
			ctx->on_error("unexpected response shape "
			              "(not {\"data\":[{\"id\":...}]})",
			    ctx->user);
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
		ctx->on_error(
		    error_msg ? error_msg : "request failed", ctx->user);
	free(ctx);
}

int
clm_agent_list_models(struct clm_agent *agent,
    void (*on_models)(char **ids, void *user),
    void (*on_error)(const char *msg, void *user), void *user)
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
	        models_list_success_cb, models_list_error_cb, NULL, ctx,
	        NULL) != 0) {
		free(ctx);
		return -EIO;
	}
	return 0;
}

int
clm_agent_probe_models(struct clm_agent *agent, const char *base_url,
    enum clm_provider provider, const char *api_key,
    void (*on_models)(char **ids, void *user),
    void (*on_error)(const char *msg, void *user), void *user)
{
	const struct clm_provider_ops *ops = clm_provider_ops_get(provider);
	autofree char *url = NULL;
	autofreev char **auth_headers = NULL;
	struct clm_llm *tmp_llm = NULL;
	struct clm_http_req req = {
	    .url = NULL,
	    .api_key = api_key,
	    .body = NULL,
	    .headers = NULL,
	    .client_suffix = NULL,
	};
	struct models_list_ctx *ctx;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(base_url != NULL, -EINVAL);

	url = clm_derive_models_url(base_url);
	if (url == NULL)
		return -ENOMEM;
	req.url = url;

	/* Same trick as agent_http_post()'s own auth-header handling, but
	 * against a throwaway clm_llm rather than the agent's live one --
	 * build_auth_headers() only reads fields out of it synchronously
	 * (e.g. anthropic_build_auth_headers() copies api_key into the
	 * header string), so it doesn't need to outlive this call. */
	if (ops->build_auth_headers != NULL &&
	    clm_llm_new(&tmp_llm, provider, api_key, base_url, "", false) ==
	        0) {
		auth_headers = ops->build_auth_headers(tmp_llm);
		clm_llm_free(tmp_llm);
		if (auth_headers != NULL) {
			req.api_key = NULL;
			req.headers = (const char *const *)auth_headers;
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return -ENOMEM;
	ctx->on_models = on_models;
	ctx->on_error = on_error;
	ctx->user = user;

	if (agent->host->http_post(agent->host->ctx, &req,
	        models_list_success_cb, models_list_error_cb, NULL, ctx,
	        NULL) != 0) {
		free(ctx);
		return -EIO;
	}
	return 0;
}

const char *
clm_agent_get_base_url(struct clm_agent *agent)
{
	return agent != NULL && agent->llm != NULL ? agent->llm->base_url
	                                           : NULL;
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

	/* A prior cancel is still unwinding: inflight/active_batch below stay
	 * non-NULL until their async teardown actually completes (a killed
	 * subprocess takes a moment to die), so without this guard a caller
	 * that re-issues cancel while waiting (e.g. impatient repeated
	 * Escape presses) would see success every time and re-signal an
	 * already-cancelling batch on each call -- harmless at the tool
	 * layer (SIGTERM twice is a no-op), but misleads a caller that
	 * reports success as "cancelled" into printing that once per
	 * keypress instead of once per actual cancel. */
	if (agent->cancelling)
		return -EALREADY;

	/*
	 * Parked waiting out a rate limit. Nothing is in flight to abort, so
	 * drop the timer and retire what it was holding: the turn it would
	 * have resent, or the compaction body it would have sent again.
	 */
	if (agent->llm_rl_timer != NULL || agent->compact_rl_timer != NULL) {
		if (agent->llm_rl_timer != NULL) {
			agent->host->timer_cancel(agent->llm_rl_timer);
			agent->llm_rl_timer = NULL;
		}
		if (agent->compact_rl_timer != NULL) {
			agent->host->timer_cancel(agent->compact_rl_timer);
			agent->compact_rl_timer = NULL;
		}
		if (agent->rl_parked_turn != NULL) {
			clm_async_turn_free(agent->rl_parked_turn);
			agent->rl_parked_turn = NULL;
		}
		compact_done(agent);
		agent->compact_resume_chain = false;
		agent->state = CLM_STATE_COMPLETE;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ECANCELED);
		return 0;
	}

	if (agent->inflight != NULL) {
		/* Waiting on the model: abort the request. Its error callback
		 * fires on_turn_done(-ECANCELED) and clears inflight. Mark the
		 * cancel so any buffered stream data is dropped rather than
		 * rendered (a normal completion also nulls inflight, so the
		 * flag is what distinguishes the two). */
		struct clm_http_call *req = agent->inflight;
		agent->inflight = NULL;
		agent->cancelling = true;
		agent->host->http_cancel(req);
		return 0;
	}
	if (agent->active_batch != NULL) {
		/* Running tools: signal them to abort; when the batch finishes
		 * unwinding, clm_agent_tools_done ends the turn as cancelled.
		 */
		agent->cancelling = true;
		clm_tools_cancel(agent);
		return 0;
	}
	return -EINVAL; /* nothing in flight */
}

/*
 * Reset conversation history to a fresh, single-system-message state --
 * what a new clm_agent_new() with the same cfg->system_prompt would start
 * with, current-time stamp re-derived. History, tools, and the provider
 * connection are otherwise independent (contrast clm_agent_set_provider,
 * which swaps the connection but leaves history untouched): this only
 * ever touches agent->history.
 */
int
clm_agent_set_effort(struct clm_agent *agent, const char *effort)
{
	ASSERT_RETURN(agent != NULL && agent->llm != NULL, -EINVAL);
	return clm_llm_set_effort(agent->llm, effort);
}

const char *
clm_agent_get_effort(const struct clm_agent *agent)
{
	if (agent == NULL || agent->llm == NULL)
		return NULL;
	return agent->llm->effort;
}

const struct clm_history *
clm_agent_get_history(const struct clm_agent *agent)
{
	return agent != NULL ? &agent->history : NULL;
}

int
clm_agent_clear_history(struct clm_agent *agent)
{
	const char *base;
	autofree char *sys = NULL;

	ASSERT_RETURN(agent != NULL, -EINVAL);

	/* Same busy gate as clm_agent_set_provider() -- see its comment for
	 * why IDLE/COMPLETE/ERROR are all fine to act on but a turn actually
	 * in flight is not (history may be referenced mid-build/mid-dispatch).
	 */
	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL ||
	    agent->state == CLM_STATE_RATE_LIMITED)
		return -EBUSY;

	base = agent->system_prompt_base ? agent->system_prompt_base
	                                 : default_system_prompt;
	sys = build_system_prompt(base, agent->system_prompt_suffix);
	if (sys == NULL)
		return -ENOMEM;

	clm_history_free(&agent->history);
	clm_history_init(&agent->history);
	clm_agent_chain_reset(agent);
	{
		struct clm_message *m = clm_history_add_system(
		    &agent->history, sys, agent->compressor);
		if (m == NULL)
			return -ENOMEM;
		clm_agent_emit_message(agent, m);
	}

	agent->last_time_stamp = time(NULL);
	return 0;
}

void
clm_agent_emit_message(struct clm_agent *agent, const struct clm_message *m)
{
	if (agent == NULL || m == NULL || agent->cb_on_message == NULL)
		return;
	agent->cb_on_message(m, agent->cb_user);
}

int
clm_agent_restore_history(struct clm_agent *agent, const struct clm_history *h)
{
	const struct clm_message *m;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(h != NULL, -EINVAL);

	if (agent->state == CLM_STATE_THINKING ||
	    agent->state == CLM_STATE_CALLING_TOOL ||
	    agent->state == CLM_STATE_RATE_LIMITED)
		return -EBUSY;

	clm_agent_chain_reset(agent);

	TAILQ_FOREACH(m, h, entries)
	{
		if (m->role == CLM_ROLE_SYSTEM)
			continue;

		/* Round-trip through the lossless JSON form: it already
		 * handles every role shape (tool_calls, tool_name, ...) and
		 * decompress/recompress across differing compressors. */
		json_cleanup cJSON *obj = clm_message_to_json_full(m, NULL);
		if (obj == NULL)
			return -ENOMEM;
		int r = clm_message_from_json(
		    &agent->history, obj, agent->compressor);
		if (r < 0)
			return r;
	}

	/*
	 * The log is the raw transcript, so a volatile tool's superseded
	 * results come back at full size -- a resumed console-driving session
	 * reloads every stale screen it had already stubbed. Apply the policy
	 * again here, keeping the newest result of each such tool.
	 */
	if (agent->volatile_tools != NULL) {
		const char *const *pat;

		for (pat = agent->volatile_tools; *pat != NULL; pat++) {
			const struct clm_message *msg;

			TAILQ_FOREACH(msg, &agent->history, entries)
			{
				char stub[128];

				if (msg->role != CLM_ROLE_TOOL ||
				    msg->tool_name == NULL ||
				    fnmatch(*pat, msg->tool_name, 0) != 0)
					continue;
				(void)snprintf(stub, sizeof(stub),
				    "[superseded by newer %s]", msg->tool_name);
				(void)clm_history_supersede_tool(
				    &agent->history, msg->tool_name, stub);
			}
		}
	}

	return 0;
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
	new_key = strdup(cfg->api_key != NULL ? cfg->api_key : "");
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
	agent->llm->disable_parallel_tool_calls =
	    cfg->disable_parallel_tool_calls;

	free(agent->models_url);
	free(agent->props_url);
	agent->models_url = new_models_url;
	agent->props_url = new_props_url;

	/* A different model/provider may well support tools even if the one
	 * just switched away from didn't -- don't carry the previous
	 * model's limitation forward onto an untested one. */
	agent->tools_unsupported = false;

	/* Reset context info: a new server/model may have different limits,
	 * unless the new model/provider supplies an explicit override. */
	agent->ctx_max = cfg->context_size > 0 ? cfg->context_size : 0;
	agent->autocompact_pct =
	    cfg->autocompact_pct > 0 ? cfg->autocompact_pct : 0;

	/* Only rebuild the LLM rate limiter if the new provider actually
	 * overrides it; otherwise keep the bucket (and its accumulated
	 * state) as-is rather than resetting it to the defaults. */
	if (cfg->rate_tokens_per_sec > 0 || cfg->rate_burst > 0) {
		int64_t rps = cfg->rate_tokens_per_sec > 0
		    ? cfg->rate_tokens_per_sec
		    : CLM_DEFAULT_LLM_RL_TOKENS_PER_SEC;
		int64_t burst = cfg->rate_burst > 0 ? cfg->rate_burst
		                                    : CLM_DEFAULT_LLM_RL_BURST;
		struct clm_ratelimit *new_rl;

		if (clm_ratelimit_new(&new_rl, (size_t)rps, (size_t)burst) ==
		    0) {
			clm_ratelimit_free(agent->llm_rl);
			agent->llm_rl = new_rl;
		}
	}

	clm_agent_chain_reset(
	    agent); /* another server, or another model: no chain */
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
	agent->rl_parked_turn = NULL;

	size_t est_tokens =
	    (turn->ctx_bytes ? turn->ctx_bytes : strlen(turn->body)) / 4;
	if (est_tokens == 0)
		est_tokens = 1;
	clm_ratelimit_consume(agent->llm_rl, est_tokens);
	agent_http_post(agent, agent->llm->base_url, turn->body,
	    clm_http_success_cb_wrapper, clm_http_error_cb_wrapper,
	    turn->streaming ? clm_http_data_cb_wrapper : NULL, turn,
	    &agent->inflight);
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
	/* Estimate token cost from the conversation this request stands for,
	 * not the bytes uploaded: a continued chain ships a few hundred bytes
	 * while the server still processes the whole context. */
	size_t est_tokens =
	    (turn->ctx_bytes ? turn->ctx_bytes : strlen(turn->body)) / 4;
	if (est_tokens == 0)
		est_tokens = 1;

	if (agent->llm_rl == NULL || agent->host->timer_set == NULL ||
	    clm_ratelimit_allow(agent->llm_rl, est_tokens)) {
		/* Unlimited, no timer available to defer with, or allowed:
		 * dispatch now (clm_ratelimit_allow already consumed the
		 * token in the allowed case). */
		agent_http_post(agent, agent->llm->base_url, turn->body,
		    clm_http_success_cb_wrapper, clm_http_error_cb_wrapper,
		    turn->streaming ? clm_http_data_cb_wrapper : NULL, turn,
		    &agent->inflight);
		return;
	}

	{
		uint64_t delay_us =
		    clm_ratelimit_delay(agent->llm_rl, est_tokens);
		uint64_t delay_ms = delay_us / 1000;
		if (delay_ms == 0)
			delay_ms = 1;
		agent->state = CLM_STATE_RATE_LIMITED;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent->host->timer_set(agent->host->ctx, delay_ms,
		    on_llm_rl_timer, turn, &agent->llm_rl_timer);
		agent->rl_parked_turn = turn;
	}
}

static void
clm_agent_start_turn(struct clm_agent *agent)
{
	const struct clm_provider_ops *ops =
	    clm_provider_ops_get(agent->llm->provider);
	json_cleanup cJSON *req = NULL;
	cJSON *messages = NULL;
	cJSON *tools = NULL;
	struct clm_async_turn *turn;
	autofree char *body_str = NULL;
	char *body;
	size_t turn_ctx_bytes = 0;
	size_t msgs_in_history;

	messages = clm_history_to_json(&agent->history, agent->compressor);
	/* Skip building/attaching "tools" entirely once this model/provider
	 * has told us it doesn't support them (see clm_http_success_cb_wrapper)
	 * -- attaching an empty or absent list is not the same signal to every
	 * backend as never mentioning tools at all, and re-sending it every
	 * turn would just repeat the same failing request forever. Each
	 * provider's build_request treats a NULL tools the same way: no
	 * "tools"/tool-choice field at all (see provider_openai.c,
	 * provider_anthropic.c). */
	if (!agent->tools_unsupported)
		tools = clm_tools_build_schema(agent);
	if (messages == NULL || (!agent->tools_unsupported && tools == NULL)) {
		cJSON_Delete(messages);
		cJSON_Delete(tools);
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

	msgs_in_history = (size_t)cJSON_GetArraySize(messages);

	/*
	 * Everything before resp_chain_sent is already on the server, so send
	 * only what is new. Size the rate limiter from the whole history
	 * first: the body shrinks, but the tokens the server processes do
	 * not, and pacing off the body would let bursts straight through.
	 */
	agent->llm->prev_response_id = NULL;
	if (agent->resp_chain_id != NULL) {
		int total = cJSON_GetArraySize(messages);
		size_t sent = agent->resp_chain_sent;

		if (sent > 0 && (int)sent <= total) {
			autofree char *full = cJSON_PrintUnformatted(messages);
			size_t i;

			turn_ctx_bytes = full != NULL ? strlen(full) : 0;
			for (i = 0; i < sent; i++)
				cJSON_DeleteItemFromArray(messages, 0);
			agent->llm->prev_response_id = agent->resp_chain_id;
		} else {
			clm_agent_chain_reset(agent);
		}
	}

	/* build_request always takes ownership of messages/tools, translating
	 * them into this provider's wire format -- see clm/provider.h. */
	req = ops->build_request(agent->llm, messages, tools, agent->stream);
	if (req == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		agent_turn_done(agent, -ENOMEM);
		return;
	}

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
	/* What the server will hold once this request lands, and what it
	 * costs to process regardless of how little we uploaded. */
	turn->history_msgs = msgs_in_history;
	turn->ctx_bytes = turn_ctx_bytes != 0 ? turn_ctx_bytes : strlen(body);
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
				agent->cb_on_state(
				    agent->state, agent->cb_user);
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
