// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include <cJSON.h>

#include "clm/tools.h"
#include "clm/internal.h"
#include "clm/log.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

#define CLM_TOOL_OUTPUT_CAP_DEFAULT (64 * 1024)    /* bytes returned to model */
#define CLM_TOOL_OUTPUT_CAP_MAX (1024 * 1024)      /* ceiling for overrides */
#define CLM_TOOL_TIMEOUT_MAX_MS 600000u            /* 10 minutes */
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

	struct clm_timer *timer;      /* per-call timeout, via host; NULL if none */
	bool timed_out;
	bool completed;

	void (*cancel)(struct clm_tool_invocation *, void *);
	void *cancel_user;

	/* Permission gate: while awaiting a decision the invocation is parked
 *	 * (on_permission fired, invoke deferred). perm_req is the opaque handle
 *	 * that the frontend answers against; it points back to this invocation. */
	bool awaiting_perm;
	struct clm_permission_req perm_req;

	/* Rate-limit deferral: when the token bucket is empty, the invocation
 *	 * is parked on rl_timer and dispatched when tokens refill. */
	struct clm_timer *rl_timer;
};

/* A batch of tool calls dispatched together for one assistant message. */
struct clm_tool_batch {
	struct clm_agent *agent;
	struct clm_tool_invocation *inv;
	size_t n;
	size_t pending;   /* outstanding completions + a dispatch hold */
	size_t done;      /* completed calls, for progress reporting */
	int status;       /* first hard error encountered, else 0 */
};

/* ------------------------------------------------------------------ */
/* Argument helpers                                                    */
/* ------------------------------------------------------------------ */

static char *
arg_string(cJSON *args, const char *key)
{
	cJSON *v = NULL;
	if (!(v = cJSON_GetObjectItemCaseSensitive(args, key)))
		return NULL;
	if (!cJSON_IsString(v))
		return NULL;
	return strdup(v->valuestring);
}

/*
 * Expand a leading "~/" to "$HOME/". Always returns a new allocation
 * (caller frees). Returns strdup(path) if no expansion needed.
 */
static char *
expand_tilde(const char *path)
{
	const char *home;
	size_t hlen, plen;
	char *out;

	if (path == NULL)
		return NULL;
	if (path[0] != '~' || path[1] != '/')
		return strdup(path);
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return strdup(path);
	hlen = strlen(home);
	plen = strlen(path + 1); /* includes the '/' */
	out = malloc(hlen + plen + 1);
	if (out == NULL)
		return strdup(path);
	memcpy(out, home, hlen);
	memcpy(out + hlen, path + 1, plen + 1); /* copy '/' + rest + NUL */
	return out;
}

static int
arg_int(cJSON *args, const char *key, int dflt)
{
	cJSON *v = NULL;
	if (!(v = cJSON_GetObjectItemCaseSensitive(args, key)))
		return dflt;
	if (!cJSON_IsNumber(v))
		return dflt;
	return (int)v->valuedouble;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

/*
 * A "removed" node is a zombie kept alive only until its last invocation
 * finishes (see the struct clm_tool comment in clm/tools.h); it must never be
 * resolved again, so lookups skip it.
 */
static const struct clm_tool *
find_tool(const struct clm_agent *agent, const char *name)
{
	const struct clm_tool *t;
	TAILQ_FOREACH(t, &agent->tools, entries) {
		if (!t->removed && strcmp(t->name, name) == 0)
			return t;
	}
	return NULL;
}

int
clm_tool_add(struct clm_agent *agent, const struct clm_tool_def *def)
{
	struct clm_tool *t;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(def != NULL, -EINVAL);
	ASSERT_RETURN(def->name != NULL, -EINVAL);
	ASSERT_RETURN(def->invoke != NULL, -EINVAL);

	if (find_tool(agent, def->name) != NULL)
		return -EEXIST;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return -ENOMEM;

	t->name = strdup(def->name);
	t->description = def->description ? strdup(def->description) : NULL;
	t->params_schema = def->params_schema ? strdup(def->params_schema) : NULL;
	if (t->name == NULL ||
	    (def->description != NULL && t->description == NULL) ||
	    (def->params_schema != NULL && t->params_schema == NULL)) {
		free(t->name);
		free(t->description);
		free(t->params_schema);
		free(t);
		return -ENOMEM;
	}
	t->invoke = def->invoke;
	t->user = def->user;
	t->output_cap = def->output_cap;
	t->timeout_ms = def->timeout_ms;
	t->flags = def->flags;

	TAILQ_INSERT_TAIL(&agent->tools, t, entries);
	agent->tool_count++;
	return 0;
}

/* Free a node's owned strings and itself. Does not unlink or touch pending
 * invocations; callers must already know it is safe (unlinked + inflight==0,
 * or agent teardown). */
static void
tool_node_free(struct clm_tool *t)
{
	free(t->name);
	free(t->description);
	free(t->params_schema);
	free(t);
}

int
clm_tool_remove(struct clm_agent *agent, const char *name)
{
	struct clm_tool *t;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(name != NULL, -EINVAL);

	TAILQ_FOREACH(t, &agent->tools, entries) {
		if (t->removed || strcmp(t->name, name) != 0)
			continue;
		TAILQ_REMOVE(&agent->tools, t, entries);
		agent->tool_count--;
		if (t->inflight == 0)
			tool_node_free(t);
		else
			t->removed = true; /* freed once the last invocation finalizes */
		return 0;
	}
	return -ENOENT;
}

void
clm_tools_free_registry(struct clm_tool_list *tools)
{
	struct clm_tool *t, *tmp;
	if (tools == NULL)
		return;
	t = TAILQ_FIRST(tools);
	while (t != NULL) {
		tmp = TAILQ_NEXT(t, entries);
		TAILQ_REMOVE(tools, t, entries);
		tool_node_free(t);
		t = tmp;
	}
}

/* ------------------------------------------------------------------ */
/* Schema construction                                                 */
/* ------------------------------------------------------------------ */

static cJSON *
int_prop(const char *desc)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	cJSON_AddItemToObject(obj, "type", cJSON_CreateString("integer"));
	cJSON_AddItemToObject(obj, "description", cJSON_CreateString(desc));
	return obj;
}

static void
inject_prop(cJSON *props, const char *name, const char *desc)
{
	cJSON *exist = cJSON_GetObjectItemCaseSensitive(props, name);
	if (exist != NULL) {
		clm_debug("schema: skip reserved param '%s' (tool defines it)", name);
		return;
	}
	cJSON *p = int_prop(desc);
	if (p != NULL)
		cJSON_AddItemToObject(props, name, p);
}

static cJSON *
tool_schema(const struct clm_tool *t)
{
	cJSON *tool = cJSON_CreateObject();
	cJSON *func, *params, *props;

	if (tool == NULL)
		return NULL;

	cJSON_AddItemToObject(tool, "type", cJSON_CreateString("function"));

	func = cJSON_CreateObject();
	if (func == NULL) {
		cJSON_Delete(tool);
		return NULL;
	}
	cJSON_AddItemToObject(tool, "function", func);

	cJSON_AddItemToObject(func, "name", cJSON_CreateString(t->name));
	cJSON_AddItemToObject(func, "description", cJSON_CreateString(t->description ? t->description : ""));

	/* Parse the tool's parameters schema, or synthesize an empty object. */
	params = t->params_schema ? cJSON_Parse(t->params_schema) : NULL;
	if (params == NULL || cJSON_GetObjectItem(params, "type") == NULL) {
		if (params != NULL)
			cJSON_Delete(params);
		params = cJSON_CreateObject();
		if (params == NULL) {
			cJSON_Delete(tool);
			return NULL;
		}
	}
	cJSON_AddItemToObject(func, "parameters", params);

	if (cJSON_GetObjectItemCaseSensitive(params, "type") == NULL)
		cJSON_AddItemToObject(params, "type", cJSON_CreateString("object"));

	props = cJSON_GetObjectItemCaseSensitive(params, "properties");
	if (props == NULL) {
		props = cJSON_CreateObject();
		if (props == NULL) {
			cJSON_Delete(tool);
			return NULL;
		}
		cJSON_AddItemToObject(params, "properties", props);
	}

	if (t->flags & CLM_TOOL_TIMEOUT_OVERRIDABLE)
		inject_prop(props, "timeout_ms", "optional: max milliseconds before this call is aborted");
	if (t->flags & CLM_TOOL_OUTPUT_CAP_OVERRIDABLE)
		inject_prop(props, "output_cap", "optional: max bytes of output to return");

	return tool;
}

cJSON *
clm_tools_build_schema(const struct clm_agent *agent)
{
	cJSON *arr = cJSON_CreateArray();
	const struct clm_tool *t;

	if (agent == NULL)
		return NULL;

	if (arr == NULL)
		return NULL;

	TAILQ_FOREACH(t, &agent->tools, entries) {
		if (t->removed || (t->flags & CLM_TOOL_HIDDEN))
			continue;
		cJSON *sch = tool_schema(t);
		if (sch != NULL)
			cJSON_AddItemToArray(arr, sch); /* steals reference */
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

void *
clm_tool_invocation_loop(const struct clm_tool_invocation *inv)
{
	struct clm_host *host = inv ? inv->batch->agent->host : NULL;
	return host ? host->ctx : NULL;
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
	struct clm_tool *t;
	TAILQ_FOREACH(t, &agent->tools, entries) {
		if (!t->removed && strcmp(t->name, name) == 0)
			return t;
	}
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
    void (*cancel)(struct clm_tool_invocation *, void *), void *user)
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

	/* Release any live timers. The host owns the timer memory (freed on its
	 * own schedule), so the batch itself can be freed synchronously -- no
	 * embedded handles to close first. */
	for (i = 0; i < batch->n; i++) {
		if (batch->inv[i].timer != NULL) {
			agent->host->timer_cancel(batch->inv[i].timer);
			batch->inv[i].timer = NULL;
		}
		if (batch->inv[i].rl_timer != NULL) {
			agent->host->timer_cancel(batch->inv[i].rl_timer);
			batch->inv[i].rl_timer = NULL;
		}
	}
	batch_really_free(batch);
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

/*
 * Agent policy check for clm_history_supersede_tool(): does this tool's
 * output go stale the moment a newer one exists? Driven by the config's volatile_tools fnmatch(3)
 * patterns (see clm_cfg), not by the tool definition -- volatility is a property of
 * how an agent uses a tool (a map snapshot in a game loop) rather than of the tool itself.
 */
static bool
tool_is_volatile(const struct clm_agent *agent, const char *name)
{
	const char *const *pat;

	if (agent->volatile_tools == NULL || name == NULL)
		return false;
	for (pat = agent->volatile_tools; *pat != NULL; pat++) {
		if (fnmatch(*pat, name, 0) == 0)
			return true;
	}
	return false;
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
	if (inv->timer != NULL) {
		agent->host->timer_cancel(inv->timer);
		inv->timer = NULL;
	}

	/* Release this invocation's pin on its tool node; if clm_tool_remove
 *	 * already unlinked it (removed) and we were the last one holding it
 *	 * alive, free it now. See struct clm_tool in clm/tools.h. */
	if (inv->def != NULL) {
		struct clm_tool *t = (struct clm_tool *)inv->def;
		t->inflight--;
		if (t->removed && t->inflight == 0)
			tool_node_free(t);
	}

	clamped = clamp_dup(content, inv->output_cap);
	out = clamped ? clamped : (content ? content : "");

	if (agent->cb_on_tool_result)
		agent->cb_on_tool_result(inv->name, out, outcome, agent->cb_user);

	/*
	 * Stub prior results of a volatile tool before recording the fresh
	 * one, so the serialized history carries exactly one live snapshot
	 * per such tool. The stub text must be deterministic: identical
	 * bytes across turns keep the already-stubbed prefix stable for
	 * backend prompt caching.
	 */
	if (tool_is_volatile(agent, inv->name)) {
		char stub[128];
		(void)snprintf(stub, sizeof(stub),
		    "[superseded by newer %s]", inv->name);
		(void)clm_history_supersede_tool(&agent->history, inv->name,
		    stub);
	}

	if (clm_history_add_tool_result(&agent->history, inv->id, inv->name,
	    out, agent->compressor) == NULL)
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
	clm_debug("[result] %s -> %.*s", inv->name,
	    (int)(content && strlen(content) > 150 ? 150 : (content ? strlen(content) : 0)),
	    content ? content : "");
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
on_timeout(void *arg)
{
	struct clm_tool_invocation *inv = arg;

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
		cJSON *a = cJSON_Parse(inv->args);
		cJSON *v;
		if (a != NULL && cJSON_IsObject(a)) {
			if ((t->flags & CLM_TOOL_OUTPUT_CAP_OVERRIDABLE) &&
			    (v = cJSON_GetObjectItemCaseSensitive(a, "output_cap")) &&
			    cJSON_IsNumber(v)) {
				int64_t x = (int64_t)v->valuedouble;
				if (x > 0)
					cap = (size_t)x;
			}
			if ((t->flags & CLM_TOOL_TIMEOUT_OVERRIDABLE) &&
			    (v = cJSON_GetObjectItemCaseSensitive(a, "timeout_ms")) &&
			    cJSON_IsNumber(v)) {
				int64_t x = (int64_t)v->valuedouble;
				if (x > 0)
					to = (uint64_t)x;
			}
		}
		cJSON_Delete(a);
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

	/* Arm the per-call timeout if the host provides timers; otherwise the
 *	 * tool simply runs without one (a blocking transport enforces its own). */
	if (inv->timeout_ms > 0 && inv->timer == NULL &&
	    agent->host->timer_set != NULL) {
		agent->host->timer_set(agent->host->ctx, inv->timeout_ms,
		    on_timeout, inv, &inv->timer);
	}
	inv->def->invoke(inv, inv->def->user);
}

/* Rate-limit timer: fires when enough tokens have refilled. */
static void dispatch_one(struct clm_tool_invocation *inv);

static void
on_rl_timer(void *arg)
{
	struct clm_tool_invocation *inv = arg;
	struct clm_agent *agent = inv->batch->agent;

	/* Release the fired timer before dispatching. */
	if (inv->rl_timer != NULL) {
		agent->host->timer_cancel(inv->rl_timer);
		inv->rl_timer = NULL;
	}

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

	/* Permission gate (default-deny). NO_PROMPT tools run unconditionally.
 *	 * A remembered _ALWAYS decision short-circuits. Otherwise, if a handler
 *	 * is registered, park the invocation and ask; with no handler, deny --
 *	 * a frontend that wires no policy runs no gated tools. */
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
}

int
clm_tools_dispatch(struct clm_agent *agent, cJSON *tool_calls)
{
	struct clm_tool_batch *batch;
	struct clm_message *amsg;
	size_t n, i;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(tool_calls != NULL, -EINVAL);

	{
		int arr_size = cJSON_GetArraySize(tool_calls);
		if (arr_size <= 0)
			return -EINVAL;
		n = (size_t)arr_size;
	}

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
		cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
		cJSON *id = NULL, *func = NULL, *name = NULL, *args = NULL;
		autofree char *printed_args = NULL;
		const char *args_str = "{}";

		inv->batch = batch;

		if (tc != NULL) {
			id = cJSON_GetObjectItemCaseSensitive(tc, "id");
			func = cJSON_GetObjectItemCaseSensitive(tc, "function");
		}
		if (func != NULL) {
			name = cJSON_GetObjectItemCaseSensitive(func, "name");
			args = cJSON_GetObjectItemCaseSensitive(func, "arguments");
		}
		if (args != NULL && cJSON_IsString(args)) {
			args_str = args->valuestring;
		} else if (args != NULL) {
			printed_args = cJSON_PrintUnformatted(args);
			if (printed_args != NULL)
				args_str = printed_args;
		}

		inv->id = strdup(id != NULL ? cJSON_GetStringValue(id) : "");
		inv->name = strdup(name != NULL ? cJSON_GetStringValue(name) : "");
		inv->args = strdup(args_str);
		clm_debug("[tool] %s(%.*s)", inv->name,
		    (int)(strlen(args_str) > 120 ? 120 : strlen(args_str)), args_str);
		if (inv->id == NULL || inv->name == NULL || inv->args == NULL)
			batch->status = -ENOMEM;

		if (inv->id != NULL && inv->name != NULL && inv->args != NULL)
			clm_message_add_tool_call(amsg, inv->id, inv->name, inv->args);

		inv->def = find_tool(agent, inv->name ? inv->name : "");
		if (inv->def != NULL)
			((struct clm_tool *)inv->def)->inflight++;
		inv_compute_limits(inv);
	}

	/* Dispatch.
 *	 * Synchronous completers (e.g. file tools) finalize inline
 *	 * but cannot drop pending to zero thanks to the hold. */
	for (i = 0; i < n; i++) {
		struct clm_tool_invocation *inv = &batch->inv[i];
		if (clm_ratelimit_allow(agent->tool_rl, 1) ||
		    agent->host->timer_set == NULL) {
			/* Allowed, or no timer to defer with: dispatch now. */
			dispatch_one(inv);
		} else {
			/* Park until tokens refill. */
			uint64_t delay_us = clm_ratelimit_delay(agent->tool_rl, 1);
			uint64_t delay_ms = delay_us / 1000;
			if (delay_ms == 0)
				delay_ms = 1;
			agent->host->timer_set(agent->host->ctx, delay_ms,
			    on_rl_timer, inv, &inv->rl_timer);
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

static cJSON *
inv_args(const struct clm_tool_invocation *inv)
{
	return inv->args ? cJSON_Parse(inv->args) : NULL;
}

static void
tool_file_read(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup cJSON *args = inv_args(inv);
	autofree char *path = NULL;
	autoclosefile FILE *fp = NULL;
	autofree char *line = NULL;
	autofree char *out = NULL;
	size_t out_len = 0, out_cap = 0, line_cap = 0, cap;
	int offset, limit, cur = 0, shown = 0, total = 0;
	char footer[160];

	(void)user;
	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	path = arg_string(args, "path");
	if (path == NULL) {
		clm_tool_fail(inv, "missing required string argument 'path'");
		return;
	}
	{
		char *ep = expand_tilde(path);
		if (ep != NULL) {
			free(path);
			path = ep;
		}
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
	json_cleanup cJSON *args = inv_args(inv);
	autofree char *path = NULL;
	autofree char *content = NULL;
	autoclosefile FILE *fp = NULL;

	(void)user;
	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	path = arg_string(args, "path");
	content = arg_string(args, "content");
	if (path == NULL || content == NULL) {
		clm_tool_fail(inv, "write_file requires 'path' and 'content' strings");
		return;
	}
	{
		char *ep = expand_tilde(path);
		if (ep != NULL) {
			free(path);
			path = ep;
		}
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

int
clm_tools_register_builtins(struct clm_agent *agent)
{
	int r;
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
	return clm_tool_add(agent, &write_def);
}
