// SPDX-License-Identifier: ISC
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/tools.h"
#include "clm/internal.h"
#include "clm/log.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

#define CLM_TOOL_OUTPUT_CAP_DEFAULT (64 * 1024)    /* bytes returned to model */
#define CLM_TOOL_OUTPUT_CAP_MAX (1024 * 1024)      /* ceiling for overrides */
#define CLM_TOOL_TIMEOUT_MAX_MS 600000u            /* 10 minutes */
#define CLM_SHELL_DEFAULT_TIMEOUT_MS 30000u
#define CLM_READ_DEFAULT_LIMIT 200                 /* lines */

/*
 * One in-flight tool call. Owned by its batch. Completed exactly once via
 * clm_tool_complete/clm_tool_fail (or the timeout path), which records a tool
 * result into history and decrements the batch's pending count.
 */
struct clm_tool_invocation;

/* Opaque to the frontend; references the parked invocation it authorizes. */
struct clm_permission_req {
	struct clm_tool_invocation *inv;
};

static void run_invoke(struct clm_tool_invocation *inv);

struct clm_tool_invocation {
	struct clm_tool_batch *batch;
	const struct clm_tool *def;   /* resolved tool, or NULL if unknown */
	char *id;
	char *name;
	char *args;                   /* raw JSON args string */

	size_t output_cap;            /* effective for this call */
	uint64_t timeout_ms;          /* effective for this call */

	uv_timer_t timer;
	bool timer_init;
	bool timed_out;
	bool completed;

	void (*cancel)(struct clm_tool_invocation *, void *);
	void *cancel_user;

	/* Permission gate: while awaiting a decision the invocation is parked
	 * (on_permission fired, invoke deferred). perm_req is the opaque handle
	 * the frontend answers against; it points back to this invocation. */
	bool awaiting_perm;
	struct clm_permission_req perm_req;

	/* Rate-limit deferral: when the token bucket is empty, the invocation
	 * is parked on rl_timer and dispatched when tokens refill. */
	uv_timer_t rl_timer;
	bool rl_deferred;
};

/* A batch of tool calls dispatched together for one assistant message. */
struct clm_tool_batch {
	struct clm_agent *agent;
	struct clm_tool_invocation *inv;
	size_t n;
	size_t pending;   /* outstanding completions + a dispatch hold */
	size_t done;      /* completed calls, for progress reporting */
	size_t closing;   /* timer handles still being closed at teardown */
	int status;       /* first hard error encountered, else 0 */
};

/* ------------------------------------------------------------------ */
/* Argument helpers                                                    */
/* ------------------------------------------------------------------ */

static char *
arg_string(struct json_object *args, const char *key)
{
	struct json_object *v = NULL;
	if (!json_object_object_get_ex(args, key, &v))
		return NULL;
	if (json_object_get_type(v) != json_type_string)
		return NULL;
	return strdup(json_object_get_string(v));
}

static int
arg_int(struct json_object *args, const char *key, int dflt)
{
	struct json_object *v = NULL;
	if (!json_object_object_get_ex(args, key, &v))
		return dflt;
	if (json_object_get_type(v) != json_type_int)
		return dflt;
	return json_object_get_int(v);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const struct clm_tool *
find_tool(const struct clm_agent *agent, const char *name)
{
	size_t i;
	for (i = 0; i < agent->tool_count; i++)
		if (strcmp(agent->tools[i].name, name) == 0)
			return &agent->tools[i];
	return NULL;
}

int
clm_tool_add(struct clm_agent *agent, const struct clm_tool_def *def)
{
	struct clm_tool *tools, *t;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(def != NULL, -EINVAL);
	ASSERT_RETURN(def->name != NULL, -EINVAL);
	ASSERT_RETURN(def->invoke != NULL, -EINVAL);

	if (find_tool(agent, def->name) != NULL)
		return -EEXIST;

	tools = realloc(agent->tools, (agent->tool_count + 1) * sizeof(*tools));
	if (tools == NULL)
		return -ENOMEM;
	agent->tools = tools;

	t = &tools[agent->tool_count];
	memset(t, 0, sizeof(*t));
	t->name = strdup(def->name);
	t->description = def->description ? strdup(def->description) : NULL;
	t->params_schema = def->params_schema ? strdup(def->params_schema) : NULL;
	if (t->name == NULL ||
	    (def->description != NULL && t->description == NULL) ||
	    (def->params_schema != NULL && t->params_schema == NULL)) {
		free(t->name);
		free(t->description);
		free(t->params_schema);
		return -ENOMEM;
	}
	t->invoke = def->invoke;
	t->user = def->user;
	t->output_cap = def->output_cap;
	t->timeout_ms = def->timeout_ms;
	t->flags = def->flags;

	agent->tool_count++;
	return 0;
}

void
clm_tools_free_registry(struct clm_tool *tools, size_t count)
{
	size_t i;
	if (tools == NULL)
		return;
	for (i = 0; i < count; i++) {
		free(tools[i].name);
		free(tools[i].description);
		free(tools[i].params_schema);
	}
	free(tools);
}

/* ------------------------------------------------------------------ */
/* Schema construction                                                 */
/* ------------------------------------------------------------------ */

static struct json_object *
int_prop(const char *desc)
{
	json_cleanup struct json_object *p = NULL;
	struct json_object *v, *ret;

	p = json_object_new_object();
	ASSERT_RETURN(p != NULL, NULL);
	v = json_object_new_string("integer");
	ASSERT_RETURN(v != NULL, NULL);
	json_object_object_add(p, "type", v);
	v = json_object_new_string(desc);
	ASSERT_RETURN(v != NULL, NULL);
	json_object_object_add(p, "description", v);

	ret = p;
	p = NULL;
	return ret;
}

static void
inject_prop(struct json_object *props, const char *name, const char *desc)
{
	struct json_object *exist, *p;

	if (json_object_object_get_ex(props, name, &exist)) {
		clm_debug("schema: skip reserved param '%s' (tool defines it)", name);
		return;
	}
	p = int_prop(desc);
	if (p != NULL)
		json_object_object_add(props, name, p);
}

static struct json_object *
tool_schema(const struct clm_tool *t)
{
	json_cleanup struct json_object *tool = NULL;
	struct json_object *func, *params, *props, *v, *ret;

	tool = json_object_new_object();
	ASSERT_RETURN(tool != NULL, NULL);

	v = json_object_new_string("function");
	ASSERT_RETURN(v != NULL, NULL);
	json_object_object_add(tool, "type", v);

	func = json_object_new_object();
	ASSERT_RETURN(func != NULL, NULL);
	json_object_object_add(tool, "function", func);

	v = json_object_new_string(t->name);
	ASSERT_RETURN(v != NULL, NULL);
	json_object_object_add(func, "name", v);

	v = json_object_new_string(t->description ? t->description : "");
	ASSERT_RETURN(v != NULL, NULL);
	json_object_object_add(func, "description", v);

	/* Parse the tool's parameters schema, or synthesize an empty object. */
	params = t->params_schema ? json_tokener_parse(t->params_schema) : NULL;
	if (params == NULL || json_object_get_type(params) != json_type_object) {
		if (params != NULL)
			json_object_put(params);
		params = json_object_new_object();
		ASSERT_RETURN(params != NULL, NULL);
	}
	json_object_object_add(func, "parameters", params); /* now under cleanup */

	if (!json_object_object_get_ex(params, "type", &v)) {
		v = json_object_new_string("object");
		ASSERT_RETURN(v != NULL, NULL);
		json_object_object_add(params, "type", v);
	}

	if (!json_object_object_get_ex(params, "properties", &props)) {
		props = json_object_new_object();
		ASSERT_RETURN(props != NULL, NULL);
		json_object_object_add(params, "properties", props);
	}

	if (t->flags & CLM_TOOL_TIMEOUT_OVERRIDABLE)
		inject_prop(props, "timeout_ms",
		    "optional: max milliseconds before this call is aborted");
	if (t->flags & CLM_TOOL_OUTPUT_CAP_OVERRIDABLE)
		inject_prop(props, "output_cap",
		    "optional: max bytes of output to return");

	ret = tool;
	tool = NULL;
	return ret;
}

struct json_object *
clm_tools_build_schema(const struct clm_agent *agent)
{
	struct json_object *arr;
	size_t i;

	if (agent == NULL)
		return NULL;

	arr = json_object_new_array();
	if (arr == NULL)
		return NULL;

	/* json_object_array_add steals the reference; do not put afterwards.
	 * HIDDEN tools stay in the registry (invocable internally) but are not
	 * advertised to the model. */
	for (i = 0; i < agent->tool_count; i++) {
		if (agent->tools[i].flags & CLM_TOOL_HIDDEN)
			continue;
		json_object_array_add(arr, tool_schema(&agent->tools[i]));
	}

	return arr;
}

/* ------------------------------------------------------------------ */
/* Invocation accessors                                                */
/* ------------------------------------------------------------------ */

const char *
clm_tool_invocation_name(const struct clm_tool_invocation *inv)
{
	return inv ? inv->name : NULL;
}

const char *
clm_tool_invocation_args(const struct clm_tool_invocation *inv)
{
	return inv ? inv->args : NULL;
}

uv_loop_t *
clm_tool_invocation_loop(const struct clm_tool_invocation *inv)
{
	return inv ? inv->batch->agent->uv : NULL;
}

const char *
clm_permission_req_name(const struct clm_permission_req *req)
{
	return (req && req->inv) ? req->inv->name : NULL;
}

const char *
clm_permission_req_args(const struct clm_permission_req *req)
{
	return (req && req->inv) ? req->inv->args : NULL;
}

/* Mutable lookup of a registered tool by name (for recording decisions). */
static struct clm_tool *
find_tool_mut(struct clm_agent *agent, const char *name)
{
	size_t i;
	for (i = 0; i < agent->tool_count; i++)
		if (strcmp(agent->tools[i].name, name) == 0)
			return &agent->tools[i];
	return NULL;
}

int
clm_tool_permission_respond(struct clm_agent *agent,
    const struct clm_permission_req *req, enum clm_permission_decision decision)
{
	struct clm_tool_invocation *inv;
	bool allow;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(req != NULL && req->inv != NULL, -EINVAL);

	inv = req->inv;
	if (!inv->awaiting_perm)
		return -EINVAL; /* already answered, or not parked */
	inv->awaiting_perm = false;

	allow = (decision == CLM_PERM_ALLOW_ONCE ||
	    decision == CLM_PERM_ALLOW_ALWAYS);

	/* Remember _ALWAYS decisions for the rest of the session, per tool. */
	if (decision == CLM_PERM_ALLOW_ALWAYS ||
	    decision == CLM_PERM_DENY_ALWAYS) {
		struct clm_tool *t = find_tool_mut(agent, inv->name);
		if (t != NULL) {
			t->remembered = true;
			t->remember_allow = allow;
		}
	}

	if (allow)
		run_invoke(inv);
	else
		clm_tool_fail(inv, "denied by user");
	return 0;
}

size_t
clm_tool_invocation_output_cap(const struct clm_tool_invocation *inv)
{
	return inv ? inv->output_cap : 0;
}

uint64_t
clm_tool_invocation_timeout_ms(const struct clm_tool_invocation *inv)
{
	return inv ? inv->timeout_ms : 0;
}

void
clm_tool_invocation_set_cancel(struct clm_tool_invocation *inv,
    void (*cancel)(struct clm_tool_invocation *inv, void *user), void *user)
{
	if (inv == NULL)
		return;
	inv->cancel = cancel;
	inv->cancel_user = user;
}

/* ------------------------------------------------------------------ */
/* Batch teardown                                                      */
/* ------------------------------------------------------------------ */

static void
batch_really_free(struct clm_tool_batch *batch)
{
	size_t i;
	for (i = 0; i < batch->n; i++) {
		free(batch->inv[i].id);
		free(batch->inv[i].name);
		free(batch->inv[i].args);
	}
	free(batch->inv);
	free(batch);
}

static void
on_timer_closed(uv_handle_t *handle)
{
	struct clm_tool_invocation *inv = handle->data;
	struct clm_tool_batch *batch = inv->batch;
	if (--batch->closing == 0)
		batch_really_free(batch);
}

static void
batch_finalize(struct clm_tool_batch *batch)
{
	struct clm_agent *agent = batch->agent;
	int status = batch->status;
	size_t i;

	if (agent->active_batch == batch)
		agent->active_batch = NULL;

	/* Advance the turn (or end it on error). This may start a new,
	 * independent batch asynchronously; this one is torn down below. */
	clm_agent_tools_done(agent, status);

	batch->closing = 0;
	for (i = 0; i < batch->n; i++) {
		if (batch->inv[i].timer_init)
			batch->closing++;
		if (batch->inv[i].rl_deferred)
			batch->closing++;
	}

	if (batch->closing == 0) {
		batch_really_free(batch);
		return;
	}
	for (i = 0; i < batch->n; i++) {
		if (batch->inv[i].timer_init)
			uv_close((uv_handle_t *)&batch->inv[i].timer, on_timer_closed);
		if (batch->inv[i].rl_deferred)
			uv_close((uv_handle_t *)&batch->inv[i].rl_timer, on_timer_closed);
	}
}

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */

/* Copy s, truncating to cap bytes with a marker. NULL on OOM. */
static char *
clamp_dup(const char *s, size_t cap)
{
	static const char marker[] = "\n[output truncated]";
	size_t len = s ? strlen(s) : 0;
	size_t mlen = sizeof(marker) - 1;
	size_t keep;
	char *out;

	if (len <= cap)
		return strdup(s ? s : "");

	keep = cap > mlen ? cap - mlen : 0;
	out = malloc(keep + mlen + 1);
	if (out == NULL)
		return NULL;
	memcpy(out, s, keep);
	memcpy(out + keep, marker, mlen + 1);
	return out;
}

static char *
fail_wrap(const char *msg)
{
	const char *m = msg ? msg : "";
	size_t n = strlen(m) + sizeof("[tool failed: ]");
	char *s = malloc(n);
	if (s != NULL)
		(void)snprintf(s, n, "[tool failed: %s]", m);
	return s;
}

static void
inv_finalize(struct clm_tool_invocation *inv, const char *content,
    enum clm_tool_outcome outcome)
{
	struct clm_tool_batch *batch = inv->batch;
	struct clm_agent *agent = batch->agent;
	autofree char *clamped = NULL;
	const char *out;

	inv->completed = true;
	if (inv->timer_init)
		uv_timer_stop(&inv->timer);

	clamped = clamp_dup(content, inv->output_cap);
	out = clamped ? clamped : (content ? content : "");

	if (agent->cb_on_tool_result)
		agent->cb_on_tool_result(inv->name, out, outcome, agent->cb_user);

	if (clm_history_add_tool_result(&agent->history, inv->id, out) == NULL)
		batch->status = -ENOMEM;

	batch->done++;
	if (agent->cb_on_tool_batch)
		agent->cb_on_tool_batch(batch->done, batch->n, agent->cb_user);

	if (batch->pending > 0)
		batch->pending--;
	if (batch->pending == 0)
		batch_finalize(batch);
}

static void
finalize_timeout(struct clm_tool_invocation *inv)
{
	char buf[80];
	(void)snprintf(buf, sizeof(buf),
	    "[tool failed: timed out after %llu ms]",
	    (unsigned long long)inv->timeout_ms);
	inv_finalize(inv, buf, CLM_TOOL_TIMEDOUT);
}

void
clm_tool_complete(struct clm_tool_invocation *inv, const char *content)
{
	if (inv == NULL || inv->completed)
		return;
	if (inv->timed_out) {
		finalize_timeout(inv);
		return;
	}
	inv_finalize(inv, content ? content : "", CLM_TOOL_OK);
}

void
clm_tool_fail(struct clm_tool_invocation *inv, const char *msg)
{
	autofree char *wrapped = NULL;

	if (inv == NULL || inv->completed)
		return;
	if (inv->timed_out) {
		finalize_timeout(inv);
		return;
	}
	wrapped = fail_wrap(msg);
	inv_finalize(inv, wrapped ? wrapped : "[tool failed]", CLM_TOOL_FAILED);
}

static void
on_timeout(uv_timer_t *timer)
{
	struct clm_tool_invocation *inv = timer->data;

	if (inv->completed)
		return;
	inv->timed_out = true;

	if (inv->cancel != NULL) {
		/* Tool can abort its work; it will report completion, which the
		 * timed_out flag reroutes to a timeout result. */
		inv->cancel(inv, inv->cancel_user);
		return;
	}
	/* No way to cancel: abandon and report the timeout now. A later
	 * completion from the tool is ignored (completed flag). */
	finalize_timeout(inv);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void
inv_compute_limits(struct clm_tool_invocation *inv)
{
	const struct clm_tool *t = inv->def;
	size_t cap = (t != NULL && t->output_cap) ? t->output_cap
	                                           : CLM_TOOL_OUTPUT_CAP_DEFAULT;
	uint64_t to = t != NULL ? t->timeout_ms : 0;

	if (t != NULL && inv->args != NULL) {
		json_cleanup struct json_object *a = json_tokener_parse(inv->args);
		struct json_object *v;
		if (a != NULL && json_object_get_type(a) == json_type_object) {
			if ((t->flags & CLM_TOOL_OUTPUT_CAP_OVERRIDABLE) &&
			    json_object_object_get_ex(a, "output_cap", &v) &&
			    json_object_get_type(v) == json_type_int) {
				int64_t x = json_object_get_int64(v);
				if (x > 0)
					cap = (size_t)x;
			}
			if ((t->flags & CLM_TOOL_TIMEOUT_OVERRIDABLE) &&
			    json_object_object_get_ex(a, "timeout_ms", &v) &&
			    json_object_get_type(v) == json_type_int) {
				int64_t x = json_object_get_int64(v);
				if (x > 0)
					to = (uint64_t)x;
			}
		}
	}

	if (cap > CLM_TOOL_OUTPUT_CAP_MAX)
		cap = CLM_TOOL_OUTPUT_CAP_MAX;
	if (to > CLM_TOOL_TIMEOUT_MAX_MS)
		to = CLM_TOOL_TIMEOUT_MAX_MS;
	inv->output_cap = cap;
	inv->timeout_ms = to;
}

/*
 * Actually invoke the resolved tool (past the permission gate). The timeout
 * clock starts here, not at dispatch: it bounds the tool's execution, not the
 * time a user spends deciding at the permission prompt.
 */
static void
run_invoke(struct clm_tool_invocation *inv)
{
	struct clm_agent *agent = inv->batch->agent;

	if (inv->timeout_ms > 0 && !inv->timer_init) {
		uv_timer_init(agent->uv, &inv->timer);
		inv->timer.data = inv;
		inv->timer_init = true;
		uv_timer_start(&inv->timer, on_timeout, inv->timeout_ms, 0);
	}
	inv->def->invoke(inv, inv->def->user);
}

/* Rate-limit timer: fires when enough tokens have refilled. */
static void dispatch_one(struct clm_tool_invocation *inv);

static void
on_rl_timer(uv_timer_t *t)
{
	struct clm_tool_invocation *inv = t->data;
	struct clm_agent *agent = inv->batch->agent;

	/* Consume the token now (should succeed after the delay). */
	clm_ratelimit_consume(agent->tool_rl, 1);
	dispatch_one(inv);
}

static void
dispatch_one(struct clm_tool_invocation *inv)
{
	struct clm_agent *agent = inv->batch->agent;
	const struct clm_tool *t = inv->def;

	if (agent->cb_on_tool_begin)
		agent->cb_on_tool_begin(inv->name, inv->args, agent->cb_user);

	if (t == NULL) {
		clm_tool_fail(inv, "unknown tool");
		return;
	}

	/*
	 * Permission gate (default-deny). NO_PROMPT tools run unconditionally.
	 * A remembered _ALWAYS decision short-circuits. Otherwise, if a handler
	 * is registered, park the invocation and ask; with no handler, deny --
	 * a frontend that wires no policy runs no gated tools.
	 */
	if (t->flags & CLM_TOOL_NO_PROMPT) {
		run_invoke(inv);
		return;
	}
	if (t->remembered) {
		if (t->remember_allow)
			run_invoke(inv);
		else
			clm_tool_fail(inv, "denied by user (remembered)");
		return;
	}
	if (agent->cb_on_permission == NULL) {
		clm_tool_fail(inv, "denied: no permission policy");
		return;
	}
	inv->awaiting_perm = true;
	inv->perm_req.inv = inv;
	agent->cb_on_permission(&inv->perm_req, agent->cb_user);
	/* Parked: clm_tool_permission_respond resumes it (allow -> run, deny ->
	 * fail). The batch's pending count still holds this invocation. */
}

int
clm_tools_dispatch(struct clm_agent *agent, struct json_object *tool_calls)
{
	struct clm_tool_batch *batch;
	struct clm_message *amsg;
	size_t n, i;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(tool_calls != NULL, -EINVAL);

	n = json_object_array_length(tool_calls);
	if (n == 0)
		return -EINVAL;

	amsg = clm_history_add_assistant_tool_calls(&agent->history);
	if (amsg == NULL)
		return -ENOMEM;

	batch = calloc(1, sizeof(*batch));
	if (batch == NULL)
		return -ENOMEM;
	batch->agent = agent;
	batch->n = n;
	batch->pending = n + 1; /* +1 hold until all dispatched */
	batch->inv = calloc(n, sizeof(*batch->inv));
	if (batch->inv == NULL) {
		free(batch);
		return -ENOMEM;
	}

	agent->active_batch = batch;

	if (agent->cb_on_tool_batch)
		agent->cb_on_tool_batch(0, n, agent->cb_user);

	/* Build invocations and mirror the calls into history. */
	for (i = 0; i < n; i++) {
		struct clm_tool_invocation *inv = &batch->inv[i];
		struct json_object *tc = json_object_array_get_idx(tool_calls, i);
		struct json_object *id = NULL, *func = NULL, *name = NULL, *args = NULL;
		const char *args_str = "{}";

		inv->batch = batch;

		if (tc != NULL) {
			json_object_object_get_ex(tc, "id", &id);
			json_object_object_get_ex(tc, "function", &func);
		}
		if (func != NULL) {
			json_object_object_get_ex(func, "name", &name);
			json_object_object_get_ex(func, "arguments", &args);
		}
		if (args != NULL && json_object_get_type(args) == json_type_string)
			args_str = json_object_get_string(args);
		else if (args != NULL)
			args_str = json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN);

		inv->id = strdup(id != NULL ? json_object_get_string(id) : "");
		inv->name = strdup(name != NULL ? json_object_get_string(name) : "");
		inv->args = strdup(args_str);
		if (inv->id == NULL || inv->name == NULL || inv->args == NULL)
			batch->status = -ENOMEM;

		if (inv->id != NULL && inv->name != NULL && inv->args != NULL)
			clm_message_add_tool_call(amsg, inv->id, inv->name, inv->args);

		inv->def = find_tool(agent, inv->name ? inv->name : "");
		inv_compute_limits(inv);
	}

	/* Dispatch. Synchronous completers (e.g. file tools) finalize inline
	 * but cannot drop pending to zero thanks to the hold. */
	for (i = 0; i < n; i++) {
		struct clm_tool_invocation *inv = &batch->inv[i];
		if (clm_ratelimit_allow(agent->tool_rl, 1)) {
			dispatch_one(inv);
		} else {
			/* Park until tokens refill. */
			uint64_t delay_us = clm_ratelimit_delay(agent->tool_rl, 1);
			uint64_t delay_ms = delay_us / 1000;
			if (delay_ms == 0)
				delay_ms = 1;
			inv->rl_deferred = true;
			uv_timer_init(agent->uv, &inv->rl_timer);
			inv->rl_timer.data = inv;
			uv_timer_start(&inv->rl_timer, on_rl_timer, delay_ms, 0);
		}
	}

	/* Release the hold. */
	if (batch->pending > 0)
		batch->pending--;
	if (batch->pending == 0)
		batch_finalize(batch);

	return 0;
}

void
clm_tools_cancel(struct clm_agent *agent)
{
	struct clm_tool_batch *batch;
	size_t i;

	if (agent == NULL || agent->active_batch == NULL)
		return;

	/* Best effort: ask running tools to abort. Full teardown of in-flight
	 * uv handles during agent destruction is a known limitation; callers
	 * should free the agent only when no turn is in flight. */
	batch = agent->active_batch;
	for (i = 0; i < batch->n; i++) {
		struct clm_tool_invocation *inv = &batch->inv[i];
		if (!inv->completed && inv->cancel != NULL)
			inv->cancel(inv, inv->cancel_user);
	}
	clm_debug("clm_tools_cancel: batch abandoned during teardown");
}

/* ------------------------------------------------------------------ */
/* Built-in tools                                                      */
/* ------------------------------------------------------------------ */

static struct json_object *
inv_args(const struct clm_tool_invocation *inv)
{
	return inv->args ? json_tokener_parse(inv->args) : NULL;
}

static void
tool_file_read(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup struct json_object *args = inv_args(inv);
	autofree char *path = NULL;
	autoclosefile FILE *fp = NULL;
	autofree char *line = NULL;
	autofree char *out = NULL;
	size_t out_len = 0, out_cap = 0, line_cap = 0, cap;
	int offset, limit, cur = 0, shown = 0, total = 0;
	char footer[160];

	(void)user;
	if (args == NULL || json_object_get_type(args) != json_type_object) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	path = arg_string(args, "path");
	if (path == NULL) {
		clm_tool_fail(inv, "missing required string argument 'path'");
		return;
	}

	offset = arg_int(args, "offset", 1);
	limit = arg_int(args, "limit", CLM_READ_DEFAULT_LIMIT);
	if (offset < 1)
		offset = 1;
	if (limit < 1)
		limit = CLM_READ_DEFAULT_LIMIT;
	cap = inv->output_cap;

	fp = fopen(path, "re");
	if (fp == NULL) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "cannot open '%s': %s", path, strerror(errno));
		clm_tool_fail(inv, buf);
		return;
	}

	for (;;) {
		ssize_t n = getline(&line, &line_cap, fp);
		if (n < 0)
			break;
		cur++;
		total = cur;
		if (cur < offset || shown >= limit)
			continue;

		if (out_len + (size_t)n + 1 > out_cap) {
			size_t ncap = out_cap ? out_cap * 2 : 4096;
			char *p;
			while (ncap < out_len + (size_t)n + 1)
				ncap *= 2;
			if (ncap > cap)
				ncap = cap;
			p = realloc(out, ncap);
			if (p == NULL) {
				clm_tool_fail(inv, "out of memory");
				return;
			}
			out = p;
			out_cap = ncap;
		}
		if (out_len + (size_t)n + 1 > cap)
			break;
		memcpy(out + out_len, line, (size_t)n);
		out_len += (size_t)n;
		out[out_len] = '\0';
		shown++;
	}

	if (shown == 0) {
		char buf[128];
		(void)snprintf(buf, sizeof(buf),
		    "(file has %d lines; offset %d is past end)", total, offset);
		clm_tool_complete(inv, buf);
		return;
	}

	(void)snprintf(footer, sizeof(footer), "\n[lines %d-%d of %d%s]",
	    offset, offset + shown - 1, total,
	    (offset + shown - 1 < total) ? "; raise offset to continue" : "");
	{
		size_t flen = strlen(footer);
		char *p = realloc(out, out_len + flen + 1);
		if (p == NULL) {
			clm_tool_fail(inv, "out of memory");
			return;
		}
		out = p;
		memcpy(out + out_len, footer, flen + 1);
	}

	clm_tool_complete(inv, out);
}

static void
tool_file_write(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup struct json_object *args = inv_args(inv);
	autofree char *path = NULL;
	autofree char *content = NULL;
	autoclosefile FILE *fp = NULL;

	(void)user;
	if (args == NULL || json_object_get_type(args) != json_type_object) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	path = arg_string(args, "path");
	content = arg_string(args, "content");
	if (path == NULL || content == NULL) {
		clm_tool_fail(inv, "write_file requires 'path' and 'content' strings");
		return;
	}

	fp = fopen(path, "we");
	if (fp == NULL) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "cannot write '%s': %s", path, strerror(errno));
		clm_tool_fail(inv, buf);
		return;
	}
	if (fputs(content, fp) == EOF) {
		clm_tool_fail(inv, "write failed");
		return;
	}
	clm_tool_complete(inv, "ok: file written");
}

/* Shell exec via uv_spawn ($SHELL -c <command>). */
struct shell_state {
	struct clm_tool_invocation *inv;
	uv_process_t proc;
	uv_pipe_t in;         /* present only when stdin is supplied */
	uv_pipe_t out;
	uv_pipe_t err;
	uv_write_t wreq;
	char *in_buf;         /* stdin blob, kept alive across the write */
	bool has_stdin;
	char *buf;
	size_t len;
	size_t bufcap;
	int handles;          /* uv handles still open (proc + pipes) */
	int64_t exit_status;
	int term_signal;
	char *spawn_err;
};

static void
shell_append(struct shell_state *s, const char *data, size_t n)
{
	size_t cap = s->inv->output_cap;
	size_t room, take;

	if (s->len >= cap)
		return; /* full; drain and discard the rest */
	room = cap - s->len;
	take = n < room ? n : room;

	if (s->len + take + 1 > s->bufcap) {
		size_t nc = s->bufcap ? s->bufcap * 2 : 4096;
		char *p;
		while (nc < s->len + take + 1)
			nc *= 2;
		if (nc > cap + 1)
			nc = cap + 1;
		p = realloc(s->buf, nc);
		if (p == NULL)
			return;
		s->buf = p;
		s->bufcap = nc;
	}
	memcpy(s->buf + s->len, data, take);
	s->len += take;
	s->buf[s->len] = '\0';
}

static void
shell_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static void
shell_finish(struct shell_state *s)
{
	struct clm_tool_invocation *inv = s->inv;

	if (s->spawn_err != NULL) {
		clm_tool_fail(inv, s->spawn_err);
	} else if (s->exit_status != 0 || s->term_signal != 0) {
		size_t mlen = s->len + 80;
		autofree char *msg = malloc(mlen);
		if (msg != NULL) {
			(void)snprintf(msg, mlen, "%s%s(exit status %lld%s)",
			    s->len ? s->buf : "", s->len ? "\n" : "",
			    (long long)s->exit_status,
			    s->term_signal ? ", killed by signal" : "");
			clm_tool_fail(inv, msg);
		} else {
			clm_tool_fail(inv, "command failed");
		}
	} else {
		clm_tool_complete(inv, s->len ? s->buf : "(command produced no output)");
	}

	free(s->spawn_err);
	free(s->in_buf);
	free(s->buf);
	free(s);
}

static void
shell_on_close(uv_handle_t *handle)
{
	struct shell_state *s = handle->data;
	if (--s->handles == 0)
		shell_finish(s);
}

static void
shell_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct shell_state *s = stream->data;
	if (nread > 0)
		shell_append(s, buf->base, (size_t)nread);
	else if (nread < 0)
		uv_close((uv_handle_t *)stream, shell_on_close);
	free(buf->base);
}

static void
shell_on_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct shell_state *s = proc->data;
	s->exit_status = exit_status;
	s->term_signal = term_signal;
	uv_close((uv_handle_t *)proc, shell_on_close);
}

static void
shell_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct shell_state *s = user;
	(void)inv;
	uv_process_kill(&s->proc, SIGTERM);
}

/* stdin blob written: close the pipe so the child sees EOF. */
static void
shell_on_stdin_written(uv_write_t *req, int status)
{
	struct shell_state *s = req->data;
	(void)status;
	uv_close((uv_handle_t *)&s->in, shell_on_close);
}

static void
tool_shell_exec(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup struct json_object *args = inv_args(inv);
	autofree char *command = NULL;
	autoclose int devnull = -1;
	struct shell_state *s;
	uv_loop_t *loop = clm_tool_invocation_loop(inv);
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	const char *shell;
	char *argv[4];
	int r;

	(void)user;
	if (args == NULL || json_object_get_type(args) != json_type_object) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(inv, "missing required string argument 'command'");
		return;
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	s->inv = inv;
	s->proc.data = s;
	uv_pipe_init(loop, &s->out, 0);
	s->out.data = s;
	uv_pipe_init(loop, &s->err, 0);
	s->err.data = s;

	/* Optional stdin blob: feed it through a pipe, else use /dev/null. */
	s->in_buf = arg_string(args, "stdin");
	s->has_stdin = (s->in_buf != NULL);
	if (s->has_stdin) {
		uv_pipe_init(loop, &s->in, 0);
		s->in.data = s;
		stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
		stdio[0].data.stream = (uv_stream_t *)&s->in;
	} else {
		devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (devnull >= 0) {
			stdio[0].flags = UV_INHERIT_FD;
			stdio[0].data.fd = devnull;
		} else {
			stdio[0].flags = UV_IGNORE;
		}
	}

	shell = getenv("SHELL");
	if (shell == NULL || shell[0] == '\0')
		shell = "/bin/sh";
	argv[0] = (char *)shell;
	argv[1] = "-c";
	argv[2] = command;
	argv[3] = NULL;

	memset(&opt, 0, sizeof(opt));
	opt.file = shell;
	opt.args = argv;
	opt.exit_cb = shell_on_exit;
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&s->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&s->err;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	s->handles = s->has_stdin ? 4 : 3;
	r = uv_spawn(loop, &s->proc, &opt);
	if (r < 0) {
		s->spawn_err = strdup(uv_strerror(r));
		uv_close((uv_handle_t *)&s->proc, shell_on_close);
		uv_close((uv_handle_t *)&s->out, shell_on_close);
		uv_close((uv_handle_t *)&s->err, shell_on_close);
		if (s->has_stdin)
			uv_close((uv_handle_t *)&s->in, shell_on_close);
		return;
	}

	clm_tool_invocation_set_cancel(inv, shell_cancel, s);
	uv_read_start((uv_stream_t *)&s->out, shell_alloc, shell_read);
	uv_read_start((uv_stream_t *)&s->err, shell_alloc, shell_read);

	if (s->has_stdin) {
		uv_buf_t b = uv_buf_init(s->in_buf, (unsigned)strlen(s->in_buf));
		s->wreq.data = s;
		if (uv_write(&s->wreq, (uv_stream_t *)&s->in, &b, 1,
		    shell_on_stdin_written) < 0)
			uv_close((uv_handle_t *)&s->in, shell_on_close);
	}
}

int
clm_tools_register_builtins(struct clm_agent *agent)
{
	int r;
	const struct clm_tool_def shell_def = {
		.name = "shell_exec",
		.description = "execute a shell command and return its output",
		.params_schema =
		    "{\"type\":\"object\","
		    "\"properties\":{"
		    "\"command\":{\"type\":\"string\","
		    "\"description\":\"the shell command to execute\"},"
		    "\"stdin\":{\"type\":\"string\","
		    "\"description\":\"optional: data to write to the command's standard input\"}},"
		    "\"required\":[\"command\"]}",
		.invoke = tool_shell_exec,
		.timeout_ms = CLM_SHELL_DEFAULT_TIMEOUT_MS,
		.flags = CLM_TOOL_TIMEOUT_OVERRIDABLE | CLM_TOOL_OUTPUT_CAP_OVERRIDABLE,
	};
	const struct clm_tool_def read_def = {
		.name = "read_file",
		.description = "read a window of lines from a text file",
		.params_schema =
		    "{\"type\":\"object\","
		    "\"properties\":{"
		    "\"path\":{\"type\":\"string\",\"description\":\"path to the file\"},"
		    "\"offset\":{\"type\":\"integer\",\"description\":\"starting line, 1-indexed (default 1)\"},"
		    "\"limit\":{\"type\":\"integer\",\"description\":\"max lines to return (default 200)\"}},"
		    "\"required\":[\"path\"]}",
		.invoke = tool_file_read,
		.flags = CLM_TOOL_NO_PROMPT, /* read-only: safe to run unprompted */
	};
	const struct clm_tool_def write_def = {
		.name = "write_file",
		.description = "write content to a file, overwriting it",
		.params_schema =
		    "{\"type\":\"object\","
		    "\"properties\":{"
		    "\"path\":{\"type\":\"string\",\"description\":\"path to the file\"},"
		    "\"content\":{\"type\":\"string\",\"description\":\"content to write\"}},"
		    "\"required\":[\"path\",\"content\"]}",
		.invoke = tool_file_write,
	};

	r = clm_tool_add(agent, &read_def);
	if (r < 0)
		return r;
	r = clm_tool_add(agent, &write_def);
	if (r < 0)
		return r;
	return clm_tool_add(agent, &shell_def);
}
