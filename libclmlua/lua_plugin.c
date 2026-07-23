// SPDX-License-Identifier: ISC
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <time.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <cjson/cJSON.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/internal.h"
#include "clm/lua_plugin.h"
#include "clm/log.h"
#include "lua_internal.h"
#include "useful.h"
#include "banned.h"

/* Forward declarations for modules registered into each plugin state. */
int clm_lua_json_open(lua_State *L);
void clm_lua_push_json_value(lua_State *L, cJSON *obj);
int clm_lua_http_open(lua_State *L, struct clm_agent *agent);

#define CLM_LUA_MEM_LIMIT (8 * 1024 * 1024) /* 8 MiB per plugin */
#define CLM_LUA_EXEC_TIMEOUT_MS 30000u     /* default: 30s CPU timeout */
#define CLM_LUA_LOAD_TIMEOUT_MS 500u       /* plugin load must be quick */
#define CLM_LUA_HOOK_INTERVAL 10000        /* check deadline every N instructions */
#define CLM_LUA_HTTP_MAX_INFLIGHT 8        /* max concurrent HTTP requests */
#define CLM_LUA_HTTP_MAX_PER_CALL 128      /* max total HTTP requests per tool call */
#define CLM_LUA_JSON_DECODE_MAX (2 * 1024 * 1024) /* 2 MiB max input to json.decode */

/* Forward declaration. */
struct lua_tool_user;

/* Per-plugin resource budget and peak tracking. */
struct clm_lua_budget {
	/* Limits (set from env defaults). */
	size_t http_max_inflight;
	size_t http_max_per_call;
	size_t json_decode_max;

	/* Current counters (reset per tool call). */
	size_t http_inflight;
	size_t http_total;
	uint64_t call_start_ns;   /* wall-clock start of current call */

	/* Peak counters (lifetime of plugin, never reset). */
	size_t peak_mem;
	size_t peak_http_inflight;
	size_t peak_http_total;    /* peak per single tool call */
	uint64_t peak_call_ns;     /* longest tool call duration */
};

/* Per-plugin state. */
struct clm_lua_plugin {
	TAILQ_ENTRY(clm_lua_plugin) entry;
	lua_State *L;
	struct clm_agent *agent;
	size_t mem_used;
	size_t mem_limit;
	char *path;
	struct lua_tool_user **tool_users; /* tracked for teardown */
	size_t tool_user_count;
	size_t tool_user_cap;
	uint64_t deadline_ns; /* wall-clock deadline for current execution */
	struct clm_lua_budget budget;
	struct clm_lua_pending_list pending;
	uint64_t next_call_serial;
	bool tearing_down;

	/* every live tool coroutine, grouped by clm_lua_call. */
	struct clm_lua_coroutine_list coroutines;
};

TAILQ_HEAD(clm_lua_plugin_list, clm_lua_plugin);

/* Top-level environment holding all plugins. */
struct clm_lua_env {
	struct clm_agent *agent;
	struct clm_lua_plugin_list plugins;
	uint64_t exec_timeout_ms; /* global execution timeout */
	/* Global budget defaults. */
	size_t http_max_inflight;
	size_t http_max_per_call;
	size_t json_decode_max;
	/* Per-tool config (parsed JSON object, keyed by tool/plugin name). */
	cJSON *tool_config;
};

int
clm_lua_pending_add(struct clm_lua_plugin *plugin, lua_State *co,
    struct clm_lua_pending *pending, clm_lua_pending_teardown_fn teardown)
{
	struct clm_lua_coroutine *lco;

	if (plugin == NULL || co == NULL || pending == NULL || teardown == NULL)
		return -EINVAL;
	if (plugin->tearing_down)
		return -ECANCELED;
	lco = clm_lua_coroutine_find(co);
	if (lco == NULL || lco->plugin != plugin || lco->call->completed)
		return -ECANCELED;
	pending->plugin = plugin;
	pending->coroutine = lco;
	pending->teardown = teardown;
	TAILQ_INSERT_TAIL(&plugin->pending, pending, plugin_entry);
	TAILQ_INSERT_TAIL(&lco->pendings, pending, coroutine_entry);
	return 0;
}

struct clm_lua_plugin *
clm_lua_pending_remove(struct clm_lua_pending *pending)
{
	struct clm_lua_plugin *plugin;
	struct clm_lua_coroutine *lco;

	if (pending == NULL || pending->plugin == NULL)
		return NULL;
	plugin = pending->plugin;
	lco = pending->coroutine;
	TAILQ_REMOVE(&plugin->pending, pending, plugin_entry);
	if (lco != NULL)
		TAILQ_REMOVE(&lco->pendings, pending, coroutine_entry);
	pending->plugin = NULL;
	pending->coroutine = NULL;
	return plugin;
}

static void
clm_lua_pending_teardown_all(struct clm_lua_plugin *plugin)
{
	struct clm_lua_pending *pending;

	plugin->tearing_down = true;
	while ((pending = TAILQ_FIRST(&plugin->pending)) != NULL) {
		(void)clm_lua_pending_remove(pending);
		pending->teardown(pending);
	}
}

/* Budget helpers called from lua_http.c (defined below). */
void clm_lua_http_done(struct clm_lua_plugin *plugin);
int clm_lua_http_start(struct clm_lua_plugin *plugin);
void clm_lua_budget_report(struct clm_lua_plugin *plugin, const char *name);
int clm_lua_resume_with_deadline(struct clm_lua_plugin *plugin,
    lua_State *co, lua_State *from, int nargs, int *nresults,
    uint64_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Capped allocator                                                    */
/* ------------------------------------------------------------------ */

static void *
lua_capped_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct clm_lua_plugin *p = ud;

	/* For a brand-new allocation (ptr == NULL), Lua 5.4 passes the
	 * object's type tag in `osize`, not a real previous size -- e.g.
	 * osize=8 for a coroutine, not "8 bytes previously allocated here".
	 * Treating that as bytes-to-subtract silently corrupts mem_used on
	 * every single new allocation (it ends up mem_used - tag too low).
	 * Over thousands of allocations in a long-running session this
	 * drifts mem_used further and further below the real heap size,
	 * until the cap approves allocations the real system can no longer
	 * satisfy -- realloc() then genuinely fails outside any protected
	 * Lua call, which Lua has no way to recover from (its OOM error has
	 * nowhere to unwind to, so the default panic handler aborts()).
	 * Only ptr != NULL (an actual resize/free of a real prior block)
	 * carries a real previous size worth accounting for. */
	size_t real_osize = ptr != NULL ? osize : 0;

	if (nsize == 0) {
		p->mem_used -= real_osize;
		free(ptr);
		return NULL;
	}
	if (p->mem_used - real_osize + nsize > p->mem_limit)
		return NULL; /* refused; Lua raises OOM */
	void *np = realloc(ptr, nsize);
	if (np == NULL)
		return NULL;
	p->mem_used = p->mem_used - real_osize + nsize;
	if (p->mem_used > p->budget.peak_mem)
		p->budget.peak_mem = p->mem_used;
	return np;
}

/* ------------------------------------------------------------------ */
/* Invocation context userdata                                         */
/* ------------------------------------------------------------------ */

#define CLM_LUA_CTX_META "clm_tool_ctx"
#define CLM_LUA_TASK_META "clm_task"

enum clm_lua_task_state {
	CLM_LUA_TASK_PENDING,
	CLM_LUA_TASK_DONE,
	CLM_LUA_TASK_ERROR,
	CLM_LUA_TASK_CANCELLED,
};

struct clm_lua_wait;

struct clm_lua_task {
	struct clm_lua_call *call;
	uint64_t call_serial;
	struct clm_lua_coroutine *lco;
	struct clm_lua_wait *waiter;
	enum clm_lua_task_state state;
	int self_ref;
	int results_ref;
	int nresults;
	char *error;
	bool error_observed;
};

struct clm_lua_wait {
	struct clm_lua_pending pending;
	struct clm_lua_task **tasks;
	size_t ntasks;
	struct clm_agent *agent;
	struct clm_timer *timer;
	bool any;
};

struct lua_tool_ctx {
	struct clm_lua_call *call;
};

static void lua_call_result(struct clm_lua_call *call, bool failed,
    const char *text, struct clm_lua_coroutine *current);

static struct lua_tool_ctx *
check_ctx(lua_State *L, int idx)
{
	return luaL_checkudata(L, idx, CLM_LUA_CTX_META);
}

static int
lua_ctx_complete(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(L);
	const char *result;

	if (ctx->call->completed)
		return luaL_error(L, "tool already completed");
	if (lco == NULL || !lco->is_main)
		return luaL_error(L, "ctx:complete may only be called from the main "
		    "tool coroutine");
	result = luaL_checkstring(L, 2);
	if (ctx->call->active_coroutines > 1) {
		lua_call_result(ctx->call, true,
		    "tool completed with unawaited tasks", lco);
		return 0;
	}
	if (ctx->call->unobserved_errors != 0) {
		lua_call_result(ctx->call, true,
		    ctx->call->child_error != NULL ? ctx->call->child_error :
		    "task failed without its result being observed", lco);
		return 0;
	}
	lua_call_result(ctx->call, false, result, lco);
	return 0;
}

static int
lua_ctx_fail(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(L);
	const char *msg;

	if (ctx->call->completed)
		return luaL_error(L, "tool already completed");
	if (lco == NULL || !lco->is_main)
		return luaL_error(L, "ctx:fail may only be called from the main "
		    "tool coroutine");
	msg = luaL_checkstring(L, 2);
	lua_call_result(ctx->call, true, msg, lco);
	return 0;
}

static int
lua_ctx_args_raw(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	const char *args = ctx->call->inv != NULL ?
	    clm_tool_invocation_args(ctx->call->inv) : NULL;

	if (args == NULL)
		lua_pushstring(L, "{}");
	else
		lua_pushstring(L, args);
	return 1;
}

static int
lua_ctx_log(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	const char *msg = luaL_checkstring(L, 2);

	(void)ctx;
	clm_debug("lua plugin: %s", msg);
	return 0;
}

static const luaL_Reg ctx_methods[] = {
	{"complete", lua_ctx_complete},
	{"fail", lua_ctx_fail},
	{"args_raw", lua_ctx_args_raw},
	{"log", lua_ctx_log},
	{NULL, NULL},
};

static void
register_ctx_meta(lua_State *L)
{
	luaL_newmetatable(L, CLM_LUA_CTX_META);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, ctx_methods, 0);
	lua_pop(L, 1);
}

static int lua_task_wait(lua_State *L);
static int lua_task_result(lua_State *L);
static int lua_task_cancel_method(lua_State *L);

static struct clm_lua_task *
check_task(lua_State *L, int idx)
{
	return luaL_checkudata(L, idx, CLM_LUA_TASK_META);
}

static int
lua_task_gc(lua_State *L)
{
	struct clm_lua_task *task = check_task(L, 1);

	if (task->results_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, task->results_ref);
		task->results_ref = LUA_NOREF;
	}
	free(task->error);
	task->error = NULL;
	return 0;
}

static const luaL_Reg task_methods[] = {
	{"wait", lua_task_wait},
	{"result", lua_task_result},
	{"cancel", lua_task_cancel_method},
	{NULL, NULL},
};

static void
register_task_meta(lua_State *L)
{
	luaL_newmetatable(L, CLM_LUA_TASK_META);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, task_methods, 0);
	lua_pushcfunction(L, lua_task_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushstring(L, "clm task");
	lua_setfield(L, -2, "__name");
	lua_pop(L, 1);
}

/* ------------------------------------------------------------------ */
/* Tool invoke bridge                                                  */
/* ------------------------------------------------------------------ */

/*
 * Stored per registered Lua tool: the plugin and the Lua registry reference
 * for the invoke function.
 */
struct lua_tool_user {
	struct clm_lua_plugin *plugin;
	int invoke_ref; /* LUA_REGISTRYINDEX reference to the invoke fn */
};

/* ------------------------------------------------------------------ */
/* CPU timeout hook                                                    */
/* ------------------------------------------------------------------ */

static uint64_t
clock_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * lua_sethook count callback. Fires every CLM_LUA_HOOK_INTERVAL bytecode
 * instructions. Checks if the plugin's wall-clock deadline has been exceeded
 * and raises an error if so, unwinding to lua_resume.
 */
static void
lua_exec_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	/* Retrieve the plugin pointer from the registry. */
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_plugin");
	struct clm_lua_plugin *p = lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (p == NULL || p->deadline_ns == 0)
		return;
	if (clock_ns() > p->deadline_ns)
		luaL_error(L, "plugin exceeded execution time budget");
}

/*
 * resume a tool coroutine with a fresh execution deadline. the deadline is
 * active only while lua bytecode runs, so time suspended on async work does
 * not consume the execution budget.
 */
int
clm_lua_resume_with_deadline(struct clm_lua_plugin *plugin, lua_State *co,
    lua_State *from, int nargs, int *nresults, uint64_t timeout_ms)
{
	int rc;

	if (timeout_ms == 0)
		timeout_ms = CLM_LUA_EXEC_TIMEOUT_MS;
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(co);
	uint64_t previous_deadline = plugin->deadline_ns;
	plugin->deadline_ns = clock_ns() + timeout_ms * 1000000ULL;
	lua_sethook(co, lua_exec_hook, LUA_MASKCOUNT, CLM_LUA_HOOK_INTERVAL);
	if (lco != NULL)
		lco->running = true;
	rc = lua_resume(co, from, nargs, nresults);
	if (lco != NULL)
		lco->running = false;
	plugin->deadline_ns = previous_deadline;
	lua_sethook(co, NULL, 0, 0);
	return rc;
}

static void lua_task_cancel_internal(struct clm_lua_task *task,
    const char *reason);
static void lua_wait_task_done(struct clm_lua_task *task);

/* register a coroutine while its thread object is on owner's stack. */
struct clm_lua_coroutine *
clm_lua_coroutine_register(struct clm_lua_plugin *plugin,
    struct clm_lua_call *call, lua_State *owner, lua_State *co, bool is_main)
{
	struct clm_lua_coroutine *lco;

	if (plugin == NULL || call == NULL || owner == NULL || co == NULL ||
	    lua_tothread(owner, -1) != co)
		return NULL;
	lco = calloc(1, sizeof(*lco));
	if (lco == NULL)
		return NULL;
	lco->plugin = plugin;
	lco->call = call;
	lco->co = co;
	lco->main_L = plugin->L;
	lco->agent = plugin->agent;
	lco->is_main = is_main;
	TAILQ_INIT(&lco->pendings);

	/* lua_newthread left the thread on owner; copy and root that value. */
	lua_pushvalue(owner, -1);
	lco->ref = luaL_ref(owner, LUA_REGISTRYINDEX);
	TAILQ_INSERT_TAIL(&plugin->coroutines, lco, entry);
	call->active_coroutines++;
	return lco;
}

void
clm_lua_coroutine_unregister(struct clm_lua_coroutine *lco)
{
	struct clm_lua_call *call;

	if (lco == NULL)
		return;
	while (!TAILQ_EMPTY(&lco->pendings)) {
		struct clm_lua_pending *pending = TAILQ_FIRST(&lco->pendings);
		(void)clm_lua_pending_remove(pending);
		pending->teardown(pending);
	}
	if (lco->task != NULL) {
		lua_task_cancel_internal(lco->task, "task cancelled");
		lco->task = NULL;
	}
	call = lco->call;
	luaL_unref(lco->plugin->L, LUA_REGISTRYINDEX, lco->ref);
	TAILQ_REMOVE(&lco->plugin->coroutines, lco, entry);
	if (call != NULL && call->active_coroutines > 0)
		call->active_coroutines--;
	free(lco);
}

struct clm_lua_coroutine *
clm_lua_coroutine_find(lua_State *co)
{
	struct clm_lua_plugin *plugin;
	struct clm_lua_coroutine *lco;

	if (co == NULL)
		return NULL;
	lua_getfield(co, LUA_REGISTRYINDEX, "_clm_plugin");
	plugin = lua_touserdata(co, -1);
	lua_pop(co, 1);
	if (plugin == NULL)
		return NULL;
	TAILQ_FOREACH(lco, &plugin->coroutines, entry) {
		if (lco->co == co)
			return lco;
	}
	return NULL;
}

bool
clm_lua_coroutine_is_valid(lua_State *co)
{
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(co);

	return lco != NULL && lco->call != NULL && !lco->call->completed &&
	    lco->call->inv != NULL;
}

static void
lua_call_free(struct clm_lua_call *call)
{
	if (call == NULL)
		return;
	if (!call->reported) {
		call->reported = true;
		clm_lua_budget_report(call->plugin,
		    call->tool_name != NULL ? call->tool_name : "?");
	}
	lua_gc(call->plugin->L, LUA_GCSTEP, 0);
	free(call->child_error);
	free(call->tool_name);
	free(call);
}

/* cancel every suspended coroutine in a family, preserving running frames. */
static void
lua_call_cancel_suspended(struct clm_lua_call *call,
    struct clm_lua_coroutine *current)
{
	struct clm_lua_coroutine *lco;

	for (;;) {
		lco = NULL;
		TAILQ_FOREACH(lco, &call->plugin->coroutines, entry) {
			if (lco->call == call && lco != current && !lco->running)
				break;
		}
		if (lco == NULL)
			break;
		clm_lua_coroutine_unregister(lco);
	}
}

static void
lua_call_result(struct clm_lua_call *call, bool failed, const char *text,
    struct clm_lua_coroutine *current)
{
	struct clm_tool_invocation *inv;

	if (call == NULL || call->completed)
		return;
	call->completed = true;
	inv = call->inv;
	call->inv = NULL;
	if (inv != NULL)
		clm_tool_invocation_set_cancel(inv, NULL, NULL);
	lua_call_cancel_suspended(call, current);
	if (inv != NULL) {
		if (failed)
			clm_tool_fail(inv, text != NULL ? text : "lua runtime error");
		else
			clm_tool_complete(inv, text != NULL ? text : "");
	}
	if (call->active_coroutines == 0)
		lua_call_free(call);
}

static void
lua_call_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct clm_lua_call *call = user;

	if (call == NULL || call->inv != inv)
		return;
	call->completed = true;
	call->inv = NULL;
	lua_call_cancel_suspended(call, NULL);
	if (call->active_coroutines == 0)
		lua_call_free(call);
}

static void
lua_task_release_root(struct clm_lua_task *task)
{
	if (task->self_ref == LUA_NOREF)
		return;
	luaL_unref(task->call->plugin->L, LUA_REGISTRYINDEX, task->self_ref);
	task->self_ref = LUA_NOREF;
}

static void
lua_task_cancel_internal(struct clm_lua_task *task, const char *reason)
{
	if (task == NULL || task->state != CLM_LUA_TASK_PENDING)
		return;
	task->state = CLM_LUA_TASK_CANCELLED;
	task->lco = NULL;
	free(task->error);
	task->error = strdup(reason != NULL ? reason : "task cancelled");
	lua_task_release_root(task);
}

static int
lua_task_store_results(struct clm_lua_task *task, lua_State *co, int nresults)
{
	int first;

	if (nresults < 0 || nresults > lua_gettop(co))
		return -EINVAL;
	first = lua_gettop(co) - nresults + 1;
	lua_createtable(co, nresults, 0);
	for (int i = 0; i < nresults; i++) {
		lua_pushvalue(co, first + i);
		lua_rawseti(co, -2, (lua_Integer)i + 1);
	}
	task->nresults = nresults;
	task->results_ref = luaL_ref(co, LUA_REGISTRYINDEX);
	return 0;
}

static void
lua_task_push_value(lua_State *L, struct clm_lua_task *task)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, task->results_ref);
	if (task->nresults == 1) {
		lua_rawgeti(L, -1, 1);
		lua_remove(L, -2);
	}
}

static void
lua_task_mark_error_observed(struct clm_lua_task *task)
{
	struct clm_lua_call *call;

	if (task->state != CLM_LUA_TASK_ERROR || task->error_observed)
		return;
	task->error_observed = true;
	call = task->call;
	if (call->unobserved_errors > 0)
		call->unobserved_errors--;
	if (call->unobserved_errors == 0) {
		free(call->child_error);
		call->child_error = NULL;
	}
}

static void
lua_task_push_outcome(lua_State *L, struct clm_lua_task *task)
{
	lua_createtable(L, 0, 3);
	if (task->state == CLM_LUA_TASK_DONE) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "ok");
		if (task->nresults != 0) {
			lua_task_push_value(L, task);
			lua_setfield(L, -2, "value");
		}
		return;
	}
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "ok");
	lua_pushstring(L, task->error != NULL ? task->error : "task failed");
	lua_setfield(L, -2, "error");
	if (task->state == CLM_LUA_TASK_CANCELLED) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "cancelled");
	}
}

static struct clm_lua_coroutine *
lua_task_main_caller(lua_State *L, const char *operation)
{
	struct clm_lua_coroutine *lco;

	if (!clm_lua_coroutine_is_valid(L))
		luaL_error(L, "%s may only be called from within a tool coroutine",
		    operation);
	lco = clm_lua_coroutine_find(L);
	if (!lco->is_main)
		luaL_error(L, "%s may only be called from the main tool coroutine",
		    operation);
	return lco;
}

static void
lua_task_check_owner(lua_State *L, struct clm_lua_task *task,
    struct clm_lua_coroutine *caller, const char *operation)
{
	if (task->call != caller->call ||
	    task->call_serial != caller->call->serial)
		luaL_error(L, "%s: task belongs to another invocation", operation);
}

static void
lua_wait_detach(struct clm_lua_wait *wait)
{
	for (size_t i = 0; i < wait->ntasks; i++) {
		if (wait->tasks[i]->waiter == wait)
			wait->tasks[i]->waiter = NULL;
	}
}

static void
lua_wait_free(struct clm_lua_wait *wait)
{
	free(wait->tasks);
	free(wait);
}

static void
lua_wait_teardown(struct clm_lua_pending *pending)
{
	struct clm_lua_wait *wait = (struct clm_lua_wait *)pending;

	lua_wait_detach(wait);
	if (wait->timer != NULL && wait->agent != NULL &&
	    wait->agent->host != NULL && wait->agent->host->timer_cancel != NULL)
		wait->agent->host->timer_cancel(wait->timer);
	lua_wait_free(wait);
}

static void
lua_wait_resume(struct clm_lua_wait *wait, size_t index, bool timed_out)
{
	struct clm_lua_coroutine *lco = wait->pending.coroutine;
	struct clm_lua_plugin *plugin;
	struct clm_lua_call *call;
	lua_State *co;
	int nargs, nresults = 0, rc;
	uint64_t timeout;

	plugin = clm_lua_pending_remove(&wait->pending);
	if (plugin == NULL || lco == NULL) {
		lua_wait_free(wait);
		return;
	}
	call = lco->call;
	co = lco->co;
	lua_wait_detach(wait);
	if (wait->timer != NULL && wait->agent != NULL &&
	    wait->agent->host != NULL && wait->agent->host->timer_cancel != NULL)
		wait->agent->host->timer_cancel(wait->timer);
	wait->timer = NULL;
	if (timed_out) {
		lua_pushnil(co);
		lua_pushstring(co, "timeout");
		nargs = 2;
	} else if (wait->any) {
		lua_pushinteger(co, (lua_Integer)index + 1);
		nargs = 1;
	} else {
		lua_pushboolean(co, 1);
		nargs = 1;
	}
	lua_wait_free(wait);
	timeout = clm_tool_invocation_timeout_ms(call->inv);
	rc = clm_lua_resume_with_deadline(plugin, co, lco->main_L, nargs,
	    &nresults, timeout);
	if (rc != LUA_YIELD) {
		const char *resume_error = rc == LUA_OK ? NULL : lua_tostring(co, -1);
		clm_lua_coroutine_finish(lco, rc, nresults, resume_error);
	}
}

static void
lua_wait_timeout(void *arg)
{
	struct clm_lua_wait *wait = arg;

	lua_wait_resume(wait, 0, true);
}

static void
lua_wait_task_done(struct clm_lua_task *task)
{
	struct clm_lua_wait *wait = task->waiter;

	if (wait == NULL)
		return;
	for (size_t i = 0; i < wait->ntasks; i++) {
		if (wait->tasks[i] == task) {
			lua_wait_resume(wait, i, false);
			return;
		}
	}
}

static int
lua_task_wait(lua_State *L)
{
	struct clm_lua_task *task = check_task(L, 1);
	struct clm_lua_coroutine *parent;
	struct clm_lua_wait *wait;
	int r;

	parent = lua_task_main_caller(L, "task:wait");
	lua_task_check_owner(L, task, parent, "task:wait");
	if (task->state != CLM_LUA_TASK_PENDING) {
		lua_pushboolean(L, 1);
		return 1;
	}
	if (task->waiter != NULL)
		return luaL_error(L, "task:wait: task is already being waited on");
	wait = calloc(1, sizeof(*wait));
	if (wait == NULL)
		return luaL_error(L, "task:wait: out of memory");
	wait->tasks = calloc(1, sizeof(*wait->tasks));
	if (wait->tasks == NULL) {
		free(wait);
		return luaL_error(L, "task:wait: out of memory");
	}
	wait->tasks[0] = task;
	wait->ntasks = 1;
	wait->agent = parent->agent;
	r = clm_lua_pending_add(parent->plugin, L, &wait->pending,
	    lua_wait_teardown);
	if (r < 0) {
		lua_wait_free(wait);
		return luaL_error(L, "task:wait: invocation is shutting down");
	}
	task->waiter = wait;
	return lua_yield(L, 0);
}

static int
lua_task_result(lua_State *L)
{
	struct clm_lua_task *task = check_task(L, 1);
	struct clm_lua_coroutine *parent;

	parent = lua_task_main_caller(L, "task:result");
	lua_task_check_owner(L, task, parent, "task:result");
	if (task->state == CLM_LUA_TASK_PENDING) {
		lua_pushnil(L);
		return 1;
	}
	lua_task_push_outcome(L, task);
	lua_task_mark_error_observed(task);
	return 1;
}

static int
lua_task_cancel_method(lua_State *L)
{
	struct clm_lua_task *task = check_task(L, 1);
	struct clm_lua_coroutine *parent;
	const char *reason = luaL_optstring(L, 2, "task cancelled");

	parent = lua_task_main_caller(L, "task:cancel");
	lua_task_check_owner(L, task, parent, "task:cancel");
	if (task->state == CLM_LUA_TASK_CANCELLED) {
		lua_pushboolean(L, 1);
		return 1;
	}
	if (task->state != CLM_LUA_TASK_PENDING || task->lco == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	clm_lua_coroutine_unregister(task->lco);
	free(task->error);
	task->error = strdup(reason);
	lua_pushboolean(L, 1);
	return 1;
}

void
clm_lua_coroutine_finish(struct clm_lua_coroutine *lco, int status,
    int nresults, const char *error)
{
	struct clm_lua_call *call;
	struct clm_lua_task *task;

	if (lco == NULL)
		return;
	call = lco->call;
	task = lco->task;
	if (task != NULL) {
		struct clm_lua_wait *waiter;

		if (status == LUA_OK && lua_task_store_results(task, lco->co,
		    nresults) == 0) {
			task->state = CLM_LUA_TASK_DONE;
		} else {
			task->state = CLM_LUA_TASK_ERROR;
			task->error = strdup(error != NULL ? error :
			    "lua task failed to store results");
			call->unobserved_errors++;
			if (call->child_error == NULL && task->error != NULL)
				call->child_error = strdup(task->error);
		}
		task->lco = NULL;
		lco->task = NULL;
		lua_task_release_root(task);
		waiter = task->waiter;
		clm_lua_coroutine_unregister(lco);
		if (waiter != NULL) {
			lua_wait_task_done(task);
			return;
		}
		if (call->active_coroutines != 0)
			return;
		if (!call->completed) {
			lua_call_result(call, true,
			    call->child_error != NULL ? call->child_error :
			    "plugin returned without calling ctx:complete/ctx:fail",
			    NULL);
			return;
		}
		lua_call_free(call);
		return;
	}

	if (status != LUA_OK && !call->completed)
		lua_call_result(call, true,
		    error != NULL ? error : "lua runtime error", lco);
	bool is_main = lco->is_main;
	clm_lua_coroutine_unregister(lco);
	if (is_main && !call->completed && call->active_coroutines != 0) {
		lua_call_result(call, true,
		    "plugin returned with unawaited tasks", NULL);
		return;
	}
	if (call->active_coroutines != 0)
		return;
	if (!call->completed) {
		lua_call_result(call, true,
		    call->child_error != NULL ? call->child_error :
		    "plugin returned without calling ctx:complete/ctx:fail", NULL);
		return;
	}
	lua_call_free(call);
}

/*
 * Log resource peaks for this plugin after a tool call completes.
 */
void
clm_lua_budget_report(struct clm_lua_plugin *plugin, const char *tool_name)
{
	struct clm_lua_budget *b = &plugin->budget;
	uint64_t elapsed_ns = clock_ns() - b->call_start_ns;

	/* Update peaks before reporting. */
	if (elapsed_ns > b->peak_call_ns)
		b->peak_call_ns = elapsed_ns;
	if (b->http_total > b->peak_http_total)
		b->peak_http_total = b->http_total;

	size_t elapsed_ms = (size_t)(elapsed_ns / 1000000ULL);
	size_t peak_ms = (size_t)(b->peak_call_ns / 1000000ULL);

	clm_debug("lua budget [%s]: call=%zums mem_peak=%zu/%zu "
	    "http=%zu http_peak_inflight=%zu peak_call=%zums",
	    tool_name,
	    elapsed_ms,
	    b->peak_mem, plugin->mem_limit,
	    b->http_total,
	    b->peak_http_inflight,
	    peak_ms);
}

/*
 * Called by lua_http.c when an HTTP request completes (success or error).
 * Decrements the in-flight counter. plugin may be NULL (defensive).
 */
void
clm_lua_http_done(struct clm_lua_plugin *plugin)
{
	if (plugin == NULL)
		return;
	if (plugin->budget.http_inflight > 0)
		plugin->budget.http_inflight--;
}

/*
 * Called by lua_http.c before starting an HTTP request. Returns 0 if allowed,
 * -1 if the budget is exceeded (caller should raise an error).
 */
int
clm_lua_http_start(struct clm_lua_plugin *plugin)
{
	struct clm_lua_budget *b;

	if (plugin == NULL)
		return 0;
	b = &plugin->budget;
	if (b->http_inflight >= b->http_max_inflight)
		return -1;
	if (b->http_total >= b->http_max_per_call)
		return -2;
	b->http_inflight++;
	b->http_total++;
	if (b->http_inflight > b->peak_http_inflight)
		b->peak_http_inflight = b->http_inflight;
	return 0;
}

/*
 * clm_tool_fn callback: called by the agent framework when a Lua tool is
 * invoked. Runs the plugin's invoke function as a coroutine so that async
 * operations (HTTP) can yield.
 */
static void
lua_tool_invoke(struct clm_tool_invocation *inv, void *user)
{
	struct lua_tool_user *tu = user;
	struct clm_lua_plugin *plugin = tu->plugin;
	lua_State *L = plugin->L;
	lua_State *co;
	struct clm_lua_call *call;
	struct clm_lua_coroutine *lco;
	struct lua_tool_ctx *ctx;
	int nres, rc;

	plugin->budget.http_total = 0;
	plugin->budget.call_start_ns = clock_ns();
	if (plugin->mem_used + 262144 > plugin->mem_limit) {
		clm_tool_fail(inv, "plugin is low on memory, refusing to start "
		    "a new invocation to avoid crashing");
		return;
	}

	call = calloc(1, sizeof(*call));
	if (call == NULL) {
		clm_tool_fail(inv, "failed to allocate lua invocation");
		return;
	}
	call->plugin = plugin;
	call->inv = inv;
	call->serial = ++plugin->next_call_serial;
	if (call->serial == 0)
		call->serial = ++plugin->next_call_serial;
	call->tool_name = strdup(clm_tool_invocation_name(inv));
	if (call->tool_name == NULL) {
		free(call);
		clm_tool_fail(inv, "failed to allocate lua invocation");
		return;
	}

	co = lua_newthread(L);
	if (co == NULL) {
		lua_call_free(call);
		clm_tool_fail(inv, "failed to create lua coroutine");
		return;
	}
	lco = clm_lua_coroutine_register(plugin, call, L, co, true);
	lua_pop(L, 1);
	if (lco == NULL) {
		lua_call_free(call);
		clm_tool_fail(inv, "failed to register lua coroutine");
		return;
	}
	clm_tool_invocation_set_cancel(inv, lua_call_cancel, call);

	lua_rawgeti(co, LUA_REGISTRYINDEX, tu->invoke_ref);
	const char *args_str = clm_tool_invocation_args(inv);
	cJSON *args_obj = cJSON_Parse(args_str ? args_str : "{}");
	if (args_obj == NULL) {
		lua_call_result(call, true,
		    "invalid tool arguments (malformed json)", lco);
		clm_lua_coroutine_finish(lco, LUA_OK, 0, NULL);
		return;
	}
	clm_lua_push_json_value(co, args_obj);
	cJSON_Delete(args_obj);

	ctx = lua_newuserdatauv(co, sizeof(*ctx), 0);
	ctx->call = call;
	luaL_setmetatable(co, CLM_LUA_CTX_META);

	nres = 0;
	rc = clm_lua_resume_with_deadline(plugin, co, L, 2, &nres,
	    clm_tool_invocation_timeout_ms(inv));
	if (rc == LUA_YIELD)
		return;
	const char *err = rc == LUA_OK ? NULL : lua_tostring(co, -1);
	if (rc != LUA_OK)
		clm_debug("lua plugin error: %s", err ? err : "(unknown)");
	clm_lua_coroutine_finish(lco, rc, nres, err);
}

/* ------------------------------------------------------------------ */
/* clm.tool_register(name, def_table)                                  */
/* ------------------------------------------------------------------ */

/*
 * Serialize a Lua table at the given stack index to a JSON string (for
 * params_schema). Uses the json.encode function already registered in the
 * state. Returns a malloc'd string, or NULL on failure.
 */
static char *
lua_table_to_json(lua_State *L, int idx)
{
	const char *s;
	char *out;

	if (!lua_istable(L, idx))
		return NULL;

	idx = lua_absindex(L, idx);
	lua_getglobal(L, "json");
	lua_getfield(L, -1, "encode");
	lua_remove(L, -2); /* remove json table */
	lua_pushvalue(L, idx);
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		lua_pop(L, 1);
		return NULL;
	}
	s = lua_tostring(L, -1);
	out = s ? strdup(s) : NULL;
	lua_pop(L, 1);
	return out;
}

/*
 * Lua: clm.tool_register(name, def_table)
 *   def_table = {
 *     description = "...",
 *     params_schema = { ... },  -- Lua table, serialized to JSON
 *     timeout_ms = N,
 *     invoke = function(args, ctx) ... end,
 *   }
 */
static int
lua_clm_tool_register(lua_State *L)
{
	struct clm_lua_plugin *plugin;
	struct lua_tool_user *tu;
	struct clm_tool_def def;
	const char *name, *desc;
	char *schema_json = NULL;
	int invoke_ref, r;

	/* Retrieve the plugin pointer from an upvalue. */
	plugin = lua_touserdata(L, lua_upvalueindex(1));
	if (plugin == NULL)
		return luaL_error(L, "clm.tool_register: internal error");

	name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);

	/* Extract description. */
	lua_getfield(L, 2, "description");
	desc = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);

	/* Extract and serialize params_schema. */
	lua_getfield(L, 2, "params_schema");
	if (lua_istable(L, -1))
		schema_json = lua_table_to_json(L, -1);
	lua_pop(L, 1);

	/* Extract invoke function and store a reference. */
	lua_getfield(L, 2, "invoke");
	if (!lua_isfunction(L, -1)) {
		free(schema_json);
		return luaL_error(L, "clm.tool_register: 'invoke' must be a function");
	}
	invoke_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	/* Extract optional timeout_ms. */
	lua_getfield(L, 2, "timeout_ms");
	uint64_t timeout_ms = 0;
	if (lua_isinteger(L, -1))
		timeout_ms = (uint64_t)lua_tointeger(L, -1);
	lua_pop(L, 1);

	/* Extract optional permission flags. Default is gated (default-deny);
	 * a plugin may opt a safe tool out of prompting with no_prompt, and/or
	 * hide it from the model's schema with hidden. */
	unsigned flags = 0;
	lua_getfield(L, 2, "no_prompt");
	if (lua_toboolean(L, -1))
		flags |= CLM_TOOL_NO_PROMPT;
	lua_pop(L, 1);
	lua_getfield(L, 2, "hidden");
	if (lua_toboolean(L, -1))
		flags |= CLM_TOOL_HIDDEN;
	lua_pop(L, 1);

	/* Allocate persistent tool user data. */
	tu = malloc(sizeof(*tu));
	if (tu == NULL) {
		free(schema_json);
		luaL_unref(L, LUA_REGISTRYINDEX, invoke_ref);
		return luaL_error(L, "out of memory");
	}
	tu->plugin = plugin;
	tu->invoke_ref = invoke_ref;

	/* Register with the agent. */
	memset(&def, 0, sizeof(def));
	def.name = name;
	def.description = desc;
	def.params_schema = schema_json;
	def.invoke = lua_tool_invoke;
	def.user = tu;
	def.timeout_ms = timeout_ms;
	def.flags = flags;

	r = clm_tool_add(plugin->agent, &def);
	free(schema_json);
	if (r < 0) {
		free(tu);
		luaL_unref(L, LUA_REGISTRYINDEX, invoke_ref);
		return luaL_error(L, "clm.tool_register failed: %s", strerror(-r));
	}

	clm_debug("lua: registered tool '%s' from %s", name, plugin->path);

	/* Track tu for teardown. */
	if (plugin->tool_user_count >= plugin->tool_user_cap) {
		size_t ncap = plugin->tool_user_cap ? plugin->tool_user_cap * 2 : 4;
		struct lua_tool_user **np = realloc(plugin->tool_users,
		    ncap * sizeof(*np));
		if (np != NULL) {
			plugin->tool_users = np;
			plugin->tool_user_cap = ncap;
		}
	}
	if (plugin->tool_user_count < plugin->tool_user_cap)
		plugin->tool_users[plugin->tool_user_count++] = tu;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Sandbox setup                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Filesystem primitives for plugins                                   */
/* ------------------------------------------------------------------ */

/*
 * clm.read_file(path) -> string or nil, err
 */
static int
lua_clm_read_file(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	FILE *fp = fopen(path, "re");
	if (fp == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 2;
	}

	luaL_Buffer B;
	luaL_buffinit(L, &B);
	char *buf = luaL_prepbuffer(&B);
	size_t n;
	while ((n = fread(buf, 1, LUAL_BUFFERSIZE, fp)) > 0) {
		luaL_addsize(&B, n);
		buf = luaL_prepbuffer(&B);
	}
	fclose(fp);
	luaL_pushresult(&B);
	return 1;
}

/*
 * clm.write_file(path, content) -> true or nil, err
 */
static int
lua_clm_write_file(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	size_t len;
	const char *content = luaL_checklstring(L, 2, &len);
	FILE *fp = fopen(path, "we");
	if (fp == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 2;
	}
	if (fwrite(content, 1, len, fp) != len) {
		fclose(fp);
		lua_pushnil(L);
		lua_pushstring(L, "write failed");
		return 2;
	}
	fclose(fp);
	lua_pushboolean(L, 1);
	return 1;
}

/* state for one pending clm.sleep call. */
struct lua_sleep_req {
	struct clm_lua_pending pending;
	struct clm_lua_coroutine *lco;
	struct clm_agent *agent;
	struct clm_timer *timer;
	uint64_t timeout_ms;
};

static void
lua_sleep_teardown(struct clm_lua_pending *pending)
{
	struct lua_sleep_req *sr = (struct lua_sleep_req *)pending;

	sr->lco = NULL;
	if (sr->timer != NULL && sr->agent != NULL && sr->agent->host != NULL &&
	    sr->agent->host->timer_cancel != NULL)
		sr->agent->host->timer_cancel(sr->timer);
	free(sr);
}

static void
lua_sleep_timer_cb(void *arg)
{
	struct lua_sleep_req *sr = arg;
	struct clm_lua_coroutine *lco = sr->pending.coroutine;
	int nres = 0, rc;

	if (clm_lua_pending_remove(&sr->pending) == NULL)
		return;
	if (sr->timer != NULL && sr->agent != NULL && sr->agent->host != NULL &&
	    sr->agent->host->timer_cancel != NULL)
		sr->agent->host->timer_cancel(sr->timer);
	sr->timer = NULL;
	if (lco == NULL || lua_status(lco->co) != LUA_YIELD) {
		clm_debug("clm.sleep: coroutine not yielded, skipping resume");
		free(sr);
		return;
	}
	rc = clm_lua_resume_with_deadline(lco->plugin, lco->co,
	    lco->plugin->L, 0, &nres, sr->timeout_ms);
	if (rc != LUA_YIELD) {
		const char *err = rc == LUA_OK ? NULL : lua_tostring(lco->co, -1);
		if (rc != LUA_OK)
			clm_debug("clm.sleep: coroutine error on resume: %s",
			    err ? err : "(unknown)");
		clm_lua_coroutine_finish(lco, rc, nres, err);
	}
	free(sr);
}

static int
lua_clm_sleep(lua_State *L)
{
	lua_Integer ms = luaL_checkinteger(L, 1);
	struct clm_lua_coroutine *lco;
	struct lua_sleep_req *sr;
	int r;

	if (ms < 0)
		ms = 0;
	if (ms > 30000)
		ms = 30000;
	if (!clm_lua_coroutine_is_valid(L))
		return luaL_error(L, "clm.sleep may only be called from within "
		    "a tool coroutine");
	lco = clm_lua_coroutine_find(L);
	if (lco->plugin->agent == NULL || lco->plugin->agent->host == NULL ||
	    lco->plugin->agent->host->timer_set == NULL)
		return luaL_error(L, "clm.sleep: no timer available");

	sr = calloc(1, sizeof(*sr));
	if (sr == NULL)
		return luaL_error(L, "clm.sleep: out of memory");
	sr->lco = lco;
	sr->agent = lco->plugin->agent;
	sr->timeout_ms = clm_tool_invocation_timeout_ms(lco->call->inv);
	r = clm_lua_pending_add(lco->plugin, L, &sr->pending,
	    lua_sleep_teardown);
	if (r < 0) {
		free(sr);
		return luaL_error(L, "clm.sleep: invocation is shutting down");
	}
	r = sr->agent->host->timer_set(sr->agent->host->ctx, (uint64_t)ms,
	    lua_sleep_timer_cb, sr, &sr->timer);
	if (r < 0) {
		(void)clm_lua_pending_remove(&sr->pending);
		free(sr);
		return luaL_error(L, "clm.sleep: timer_set failed");
	}
	return lua_yield(L, 0);
}

struct lua_spawn_req {
	struct clm_lua_pending pending;
	struct clm_lua_coroutine *lco;
	struct clm_agent *agent;
	struct clm_timer *timer;
	uint64_t timeout_ms;
};

static void
lua_spawn_teardown(struct clm_lua_pending *pending)
{
	struct lua_spawn_req *req = (struct lua_spawn_req *)pending;

	req->lco = NULL;
	if (req->timer != NULL && req->agent != NULL && req->agent->host != NULL &&
	    req->agent->host->timer_cancel != NULL)
		req->agent->host->timer_cancel(req->timer);
	free(req);
}

static void
lua_spawn_start(void *arg)
{
	struct lua_spawn_req *req = arg;
	struct clm_lua_coroutine *lco = req->pending.coroutine;
	int nresults = 0, rc;

	if (clm_lua_pending_remove(&req->pending) == NULL)
		return;
	if (req->timer != NULL && req->agent != NULL && req->agent->host != NULL &&
	    req->agent->host->timer_cancel != NULL)
		req->agent->host->timer_cancel(req->timer);
	req->timer = NULL;
	if (lco == NULL) {
		free(req);
		return;
	}
	rc = clm_lua_resume_with_deadline(lco->plugin, lco->co, lco->main_L,
	    0, &nresults, req->timeout_ms);
	if (rc != LUA_YIELD) {
		const char *error = rc == LUA_OK ? NULL : lua_tostring(lco->co, -1);
		clm_lua_coroutine_finish(lco, rc, nresults, error);
	}
	free(req);
}

static int
lua_clm_spawn(lua_State *L)
{
	struct clm_lua_coroutine *parent, *child_lco;
	struct clm_lua_task *task;
	struct lua_spawn_req *req;
	lua_State *child;
	int r;

	luaL_checktype(L, 1, LUA_TFUNCTION);
	if (!clm_lua_coroutine_is_valid(L))
		return luaL_error(L, "clm.spawn may only be called from within "
		    "a tool coroutine");
	parent = clm_lua_coroutine_find(L);
	if (!parent->is_main)
		return luaL_error(L, "clm.spawn may only be called from the main "
		    "tool coroutine");
	if (parent->agent == NULL || parent->agent->host == NULL ||
	    parent->agent->host->timer_set == NULL)
		return luaL_error(L, "clm.spawn: no scheduler available");

	task = lua_newuserdatauv(L, sizeof(*task), 0);
	memset(task, 0, sizeof(*task));
	task->call = parent->call;
	task->call_serial = parent->call->serial;
	task->state = CLM_LUA_TASK_PENDING;
	task->self_ref = LUA_NOREF;
	task->results_ref = LUA_NOREF;
	luaL_setmetatable(L, CLM_LUA_TASK_META);
	lua_pushvalue(L, -1);
	task->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	child = lua_newthread(L);
	child_lco = clm_lua_coroutine_register(parent->plugin, parent->call,
	    L, child, false);
	if (child_lco == NULL) {
		lua_pop(L, 1);
		lua_task_cancel_internal(task, "failed to register task coroutine");
		return luaL_error(L, "clm.spawn: out of memory");
	}
	task->lco = child_lco;
	child_lco->task = task;
	lua_pushvalue(L, 1);
	lua_xmove(L, child, 1);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		clm_lua_coroutine_unregister(child_lco);
		lua_pop(L, 1);
		return luaL_error(L, "clm.spawn: out of memory");
	}
	req->lco = child_lco;
	req->agent = parent->agent;
	req->timeout_ms = clm_tool_invocation_timeout_ms(parent->call->inv);
	r = clm_lua_pending_add(parent->plugin, child, &req->pending,
	    lua_spawn_teardown);
	if (r < 0) {
		free(req);
		clm_lua_coroutine_unregister(child_lco);
		lua_pop(L, 1);
		return luaL_error(L, "clm.spawn: invocation is shutting down");
	}
	r = req->agent->host->timer_set(req->agent->host->ctx, 0,
	    lua_spawn_start, req, &req->timer);
	if (r < 0) {
		(void)clm_lua_pending_remove(&req->pending);
		free(req);
		clm_lua_coroutine_unregister(child_lco);
		lua_pop(L, 1);
		return luaL_error(L, "clm.spawn: failed to schedule task");
	}
	lua_pop(L, 1);
	return 1;
}

static int
lua_clm_wait_any(lua_State *L)
{
	struct clm_lua_coroutine *parent;
	struct clm_lua_task **tasks;
	struct clm_lua_wait *wait;
	lua_Integer lua_ntasks;
	lua_Integer timeout_ms = 0;
	size_t ntasks, ready = SIZE_MAX;
	bool have_timeout = false;
	int r;

	luaL_checktype(L, 1, LUA_TTABLE);
	parent = lua_task_main_caller(L, "clm.wait_any");
	if (!lua_isnoneornil(L, 2)) {
		timeout_ms = luaL_checkinteger(L, 2);
		if (timeout_ms < 0)
			return luaL_error(L, "clm.wait_any: timeout must be nonnegative");
		have_timeout = true;
	}
	lua_ntasks = luaL_len(L, 1);
	if (lua_ntasks <= 0 || lua_ntasks > 128)
		return luaL_error(L, "clm.wait_any: expected 1 to 128 tasks");
	ntasks = (size_t)lua_ntasks;
	tasks = calloc(ntasks, sizeof(*tasks));
	if (tasks == NULL)
		return luaL_error(L, "clm.wait_any: out of memory");
	for (size_t i = 0; i < ntasks; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i + 1);
		tasks[i] = luaL_testudata(L, -1, CLM_LUA_TASK_META);
		lua_pop(L, 1);
		if (tasks[i] == NULL) {
			free(tasks);
			return luaL_error(L, "clm.wait_any: expected task at index %zu",
			    i + 1);
		}
		if (tasks[i]->call != parent->call ||
		    tasks[i]->call_serial != parent->call->serial) {
			free(tasks);
			return luaL_error(L, "clm.wait_any: task belongs to another "
			    "invocation");
		}
		if (tasks[i]->waiter != NULL) {
			free(tasks);
			return luaL_error(L, "clm.wait_any: task is already being "
			    "waited on");
		}
		for (size_t j = 0; j < i; j++) {
			if (tasks[j] == tasks[i]) {
				free(tasks);
				return luaL_error(L, "clm.wait_any: duplicate task");
			}
		}
		if (ready == SIZE_MAX &&
		    tasks[i]->state != CLM_LUA_TASK_PENDING)
			ready = i;
	}
	if (ready != SIZE_MAX) {
		free(tasks);
		lua_pushinteger(L, (lua_Integer)ready + 1);
		return 1;
	}
	wait = calloc(1, sizeof(*wait));
	if (wait == NULL) {
		free(tasks);
		return luaL_error(L, "clm.wait_any: out of memory");
	}
	wait->tasks = tasks;
	wait->ntasks = ntasks;
	wait->agent = parent->agent;
	wait->any = true;
	r = clm_lua_pending_add(parent->plugin, L, &wait->pending,
	    lua_wait_teardown);
	if (r < 0) {
		lua_wait_free(wait);
		return luaL_error(L, "clm.wait_any: invocation is shutting down");
	}
	for (size_t i = 0; i < ntasks; i++)
		tasks[i]->waiter = wait;
	if (have_timeout) {
		if (wait->agent == NULL || wait->agent->host == NULL ||
		    wait->agent->host->timer_set == NULL) {
			(void)clm_lua_pending_remove(&wait->pending);
			lua_wait_detach(wait);
			lua_wait_free(wait);
			return luaL_error(L, "clm.wait_any: no timer available");
		}
		r = wait->agent->host->timer_set(wait->agent->host->ctx,
		    (uint64_t)timeout_ms, lua_wait_timeout, wait, &wait->timer);
		if (r < 0) {
			(void)clm_lua_pending_remove(&wait->pending);
			lua_wait_detach(wait);
			lua_wait_free(wait);
			return luaL_error(L, "clm.wait_any: timer_set failed");
		}
	}
	return lua_yield(L, 0);
}

static const char task_helpers[] =
    "function clm.await_all(tasks)\n"
    "  local outcomes = {}\n"
    "  for i, task in ipairs(tasks) do\n"
    "    task:wait()\n"
    "    outcomes[i] = task:result()\n"
    "  end\n"
    "  return outcomes\n"
    "end\n"
    "function clm.try_all(tasks)\n"
    "  local pending = {}\n"
    "  local results = {}\n"
    "  for i, task in ipairs(tasks) do\n"
    "    pending[i] = { index = i, task = task }\n"
    "  end\n"
    "  while #pending ~= 0 do\n"
    "    local waiting = {}\n"
    "    for i, item in ipairs(pending) do\n"
    "      waiting[i] = item.task\n"
    "    end\n"
    "    local done, err = clm.wait_any(waiting)\n"
    "    if done == nil then return nil, err end\n"
    "    local item = table.remove(pending, done)\n"
    "    local outcome = item.task:result()\n"
    "    if not outcome.ok then\n"
    "      for _, other in ipairs(pending) do\n"
    "        other.task:cancel(\"sibling failed\")\n"
    "        other.task:result()\n"
    "      end\n"
    "      return nil, outcome.error\n"
    "    end\n"
    "    results[item.index] = outcome.value\n"
    "  end\n"
    "  return results\n"
    "end\n";

static int
install_task_helpers(lua_State *L)
{
	if (luaL_loadbuffer(L, task_helpers, sizeof(task_helpers) - 1,
	    "=clm task helpers") != LUA_OK)
		return -1;
	if (lua_pcall(L, 0, 0, 0) != LUA_OK)
		return -1;
	return 0;
}

static void
sandbox_state(lua_State *L, struct clm_lua_plugin *plugin)
{
	/* Open only the safe standard libraries. */
	luaL_requiref(L, "_G", luaopen_base, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "string", luaopen_string, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "table", luaopen_table, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "math", luaopen_math, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "utf8", luaopen_utf8, 1);
	lua_pop(L, 1);

	/* Remove unsafe globals from _G. */
	static const char *const remove[] = {
		"dofile", "loadfile", "load", "require",
		"collectgarbage", NULL,
	};
	for (const char *const *p = remove; *p != NULL; p++) {
		lua_pushnil(L);
		lua_setglobal(L, *p);
	}

	/* Register the 'clm' module. */
	lua_newtable(L);
	lua_pushlightuserdata(L, plugin);
	lua_pushcclosure(L, lua_clm_tool_register, 1);
	lua_setfield(L, -2, "tool_register");
	lua_pushcfunction(L, lua_clm_read_file);
	lua_setfield(L, -2, "read_file");
	lua_pushcfunction(L, lua_clm_write_file);
	lua_setfield(L, -2, "write_file");
	lua_pushcfunction(L, lua_clm_sleep);
	lua_setfield(L, -2, "sleep");
	lua_pushcfunction(L, lua_clm_spawn);
	lua_setfield(L, -2, "spawn");
	lua_pushcfunction(L, lua_clm_wait_any);
	lua_setfield(L, -2, "wait_any");
	lua_setglobal(L, "clm");

	/* Register userdata metatables. */
	register_ctx_meta(L);
	register_task_meta(L);

	/* Store plugin pointer in registry for the exec timeout hook. */
	lua_pushlightuserdata(L, plugin);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_plugin");

	/* Register json module. */
	clm_lua_json_open(L);

	/* Set json.decode size limit in registry. */
	lua_pushinteger(L, (lua_Integer)plugin->budget.json_decode_max);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_json_max");

	/* Register http module (sets up ctx:http_get, ctx:http_post). */
	clm_lua_http_open(L, plugin->agent);
}

/* ------------------------------------------------------------------ */
/* Plugin loading                                                      */
/* ------------------------------------------------------------------ */

static int
load_one_plugin(struct clm_lua_env *env, const char *path)
{
	struct clm_lua_plugin *plugin;
	lua_State *L;

	plugin = calloc(1, sizeof(*plugin));
	if (plugin == NULL)
		return -ENOMEM;
	plugin->agent = env->agent;
	plugin->mem_limit = CLM_LUA_MEM_LIMIT;
	plugin->budget.http_max_inflight = env->http_max_inflight;
	plugin->budget.http_max_per_call = env->http_max_per_call;
	plugin->budget.json_decode_max = env->json_decode_max;
	TAILQ_INIT(&plugin->pending);
	TAILQ_INIT(&plugin->coroutines);
	plugin->path = strdup(path);
	if (plugin->path == NULL) {
		free(plugin);
		return -ENOMEM;
	}

	/* Create a state with a capped allocator. */
	L = lua_newstate(lua_capped_alloc, plugin);
	if (L == NULL) {
		free(plugin->path);
		free(plugin);
		return -ENOMEM;
	}
	plugin->L = L;

	/* set up the sandbox and composable task helpers. */
	sandbox_state(L, plugin);
	if (install_task_helpers(L) < 0) {
		clm_debug("lua: failed to install task helpers for %s: %s", path,
		    lua_tostring(L, -1));
		lua_close(L);
		free(plugin->path);
		free(plugin);
		return -EINVAL;
	}

	/* Inject per-plugin config as clm.config (scoped by plugin name). */
	if (env->tool_config != NULL) {
		/* Derive plugin name from filename: strip directory and .lua */
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		size_t blen = strlen(base);
		char name[256];
		if (blen > 4 && blen < sizeof(name)) {
			memcpy(name, base, blen - 4);
			name[blen - 4] = '\0';
		} else {
			(void)snprintf(name, sizeof(name), "%s", base);
		}

		cJSON *pcfg = cJSON_GetObjectItemCaseSensitive(env->tool_config, name);
		if (pcfg != NULL) {
			lua_getglobal(L, "clm");
			clm_lua_push_json_value(L, pcfg);
			lua_setfield(L, -2, "config");
			lua_pop(L, 1); /* pop clm table */
		} else {
			/* No config for this plugin: clm.config = {} */
			lua_getglobal(L, "clm");
			lua_newtable(L);
			lua_setfield(L, -2, "config");
			lua_pop(L, 1);
		}
	} else {
		lua_getglobal(L, "clm");
		lua_newtable(L);
		lua_setfield(L, -2, "config");
		lua_pop(L, 1);
	}

	/* Execute the plugin file. */
	if (luaL_loadfile(L, path) != LUA_OK) {
		const char *err = lua_tostring(L, -1);
		clm_debug("lua: failed to load %s: %s", path, err ? err : "?");
		lua_close(L);
		free(plugin->path);
		free(plugin);
		return -EINVAL;
	}
	/*
	 * Bound the load phase the same way tool calls are bounded: install the
	 * count hook and a wall-clock deadline around the top-level script so a
	 * plugin that spins (e.g. `while true do end` at file scope) cannot wedge
	 * the loop at startup. Memory and the global sandbox are already in force
	 * (capped allocator at lua_newstate; sandbox_state before loadfile). Load
	 * is meant to be quick (register tools, build schemas), so it gets a much
	 * tighter deadline than a tool call.
	 */
	plugin->deadline_ns = clock_ns() + CLM_LUA_LOAD_TIMEOUT_MS * 1000000ULL;
	lua_sethook(L, lua_exec_hook, LUA_MASKCOUNT, CLM_LUA_HOOK_INTERVAL);
	int prc = lua_pcall(L, 0, 0, 0);
	plugin->deadline_ns = 0;
	lua_sethook(L, NULL, 0, 0);
	if (prc != LUA_OK) {
		const char *err = lua_tostring(L, -1);
		clm_debug("lua: error executing %s: %s", path, err ? err : "?");
		lua_close(L);
		free(plugin->path);
		free(plugin);
		return -EINVAL;
	}

	TAILQ_INSERT_TAIL(&env->plugins, plugin, entry);
	clm_debug("lua: loaded plugin %s", path);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

CLM_API int
clm_lua_env_new(struct clm_agent *agent, struct clm_lua_env **out)
{
	struct clm_lua_env *env;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(out != NULL, -EINVAL);

	env = calloc(1, sizeof(*env));
	if (env == NULL)
		return -ENOMEM;
	env->agent = agent;
	TAILQ_INIT(&env->plugins);
	env->exec_timeout_ms = CLM_LUA_EXEC_TIMEOUT_MS;
	env->http_max_inflight = CLM_LUA_HTTP_MAX_INFLIGHT;
	env->http_max_per_call = CLM_LUA_HTTP_MAX_PER_CALL;
	env->json_decode_max = CLM_LUA_JSON_DECODE_MAX;
	*out = env;
	return 0;
}

/* qsort comparator over strdup'd plugin file names. */
static int
name_cmp(const void *a, const void *b)
{
	const char *const *pa = a;
	const char *const *pb = b;
	return strcmp(*pa, *pb);
}

CLM_API int
clm_lua_load_plugins(struct clm_lua_env *env, const char *dir)
{
	DIR *d;
	struct dirent *ent;
	int loaded = 0;

	ASSERT_RETURN(env != NULL, -EINVAL);
	ASSERT_RETURN(dir != NULL, -EINVAL);

	d = opendir(dir);
	if (d == NULL) {
		if (errno == ENOENT) {
			clm_debug("lua: plugin dir '%s' not found, skipping", dir);
			return 0;
		}
		return -errno;
	}

	/*
	 * Collect the *.lua entries, then sort by name so load order is
	 * deterministic (readdir order is filesystem-defined). This matters when
	 * two plugins register the same tool name — the outcome shouldn't depend
	 * on directory layout.
	 */
	char **names = NULL;
	size_t nnames = 0, ncap = 0;
	while ((ent = readdir(d)) != NULL) {
		size_t nlen = strlen(ent->d_name);

		if (nlen < 5)
			continue;
		if (strcmp(ent->d_name + nlen - 4, ".lua") != 0)
			continue;

		if (nnames >= ncap) {
			size_t ncap2 = ncap ? ncap * 2 : 8;
			char **np = realloc(names, ncap2 * sizeof(*np));
			if (np == NULL)
				continue;
			names = np;
			ncap = ncap2;
		}
		names[nnames] = strdup(ent->d_name);
		if (names[nnames] != NULL)
			nnames++;
	}
	closedir(d);

	qsort(names, nnames, sizeof(*names), name_cmp);

	size_t dirlen = strlen(dir);
	for (size_t i = 0; i < nnames; i++) {
		size_t nlen = strlen(names[i]);
		char *path;
		struct stat st;

		/* dir + '/' + name + '\0' */
		path = malloc(dirlen + 1 + nlen + 1);
		if (path == NULL) {
			free(names[i]);
			continue;
		}
		(void)snprintf(path, dirlen + 1 + nlen + 1, "%s/%s", dir, names[i]);

		/* Only load regular files (stat follows symlinks, so a symlink to a
		 * regular file is fine; a directory named foo.lua is skipped). */
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			clm_debug("lua: skipping %s (not a regular file)", path);
			free(path);
			free(names[i]);
			continue;
		}

		int r = load_one_plugin(env, path);
		if (r < 0)
			clm_debug("lua: skipping %s (error %d)", path, r);
		else
			loaded++;
		free(path);
		free(names[i]);
	}
	free(names);

	clm_debug("lua: loaded %d plugin(s) from %s", loaded, dir);
	return 0;
}

CLM_API void
clm_lua_env_free(struct clm_lua_env *env)
{
	struct clm_lua_plugin *p, *tmp;

	if (env == NULL)
		return;
	p = TAILQ_FIRST(&env->plugins);
	while (p != NULL) {
		tmp = TAILQ_NEXT(p, entry);
		p->tearing_down = true;
		while (!TAILQ_EMPTY(&p->coroutines)) {
			struct clm_lua_coroutine *lco = TAILQ_FIRST(&p->coroutines);
			struct clm_lua_call *call = lco->call;
			if (!call->completed) {
				lua_call_result(call, true,
				    "lua plugin environment is shutting down", NULL);
			} else {
				clm_lua_coroutine_unregister(lco);
				if (call->active_coroutines == 0)
					lua_call_free(call);
			}
		}
		clm_lua_pending_teardown_all(p);
		/* Free tool user structs and unref invoke functions. */
		for (size_t j = 0; j < p->tool_user_count; j++) {
			luaL_unref(p->L, LUA_REGISTRYINDEX,
			    p->tool_users[j]->invoke_ref);
			free(p->tool_users[j]);
		}
		free(p->tool_users);
		if (p->L != NULL)
			lua_close(p->L);
		free(p->path);
		free(p);
		p = tmp;
	}
	if (env->tool_config != NULL)
		cJSON_Delete(env->tool_config);
	free(env);
}

CLM_API int
clm_lua_env_set_config(struct clm_lua_env *env, const char *tool_config_json)
{
	cJSON *obj;

	ASSERT_RETURN(env != NULL, -EINVAL);
	if (tool_config_json == NULL)
		return 0;

	obj = cJSON_Parse(tool_config_json);
	if (obj == NULL || !cJSON_IsObject(obj)) {
		if (obj != NULL)
			cJSON_Delete(obj);
		return -EINVAL;
	}
	if (env->tool_config != NULL)
		cJSON_Delete(env->tool_config);
	env->tool_config = obj;
	return 0;
}

CLM_API int
clm_lua_env_set_config_from(struct clm_lua_env *env, struct clm_lua_cfg *cfg)
{
	char *json;

	ASSERT_RETURN(env != NULL, -EINVAL);
	if (cfg == NULL)
		return 0;
	json = clm_lua_cfg_tools_json(cfg);
	if (json == NULL)
		return 0; /* no tools section is fine */
	int r = clm_lua_env_set_config(env, json);
	free(json);
	return r;
}

/* ------------------------------------------------------------------ */
/* Config state                                                        */
/* ------------------------------------------------------------------ */

struct clm_lua_cfg {
	lua_State *L;
	int cfg_ref; /* registry ref to the config table */
	int agent_ref; /* registry ref to the resolved agent table, or LUA_NOREF */
	char *resolved_agent_name; /* name actually resolved by load_agent (owned) */
};

/* Path with the last component replaced, e.g. ".../clm/config.lua" +
 * "secrets.lua" -> ".../clm/secrets.lua". NULL if path has no '/'. */
static char *
sibling_path(const char *path, const char *name)
{
	const char *slash = strrchr(path, '/');
	if (slash == NULL)
		return NULL;

	size_t dirlen = (size_t)(slash - path);
	size_t n = dirlen + 1 + strlen(name) + 1;
	char *out = malloc(n);
	if (out == NULL)
		return NULL;
	(void)snprintf(out, n, "%.*s/%s", (int)dirlen, path, name);
	return out;
}

/*
 * Loads secrets.lua (a sibling of config.lua) and pushes it as
 * clm.secrets, so config.lua and per-agent profile files -- which share
 * this lua_State -- can write e.g. api_key = clm.secrets.tavily instead
 * of a literal key. Missing or invalid secrets.lua yields an empty
 * table rather than failing config load entirely: secrets are optional.
 * Leaves a table on top of the stack (secrets), suitable for
 * lua_setfield(L, -2, "secrets") into the caller's 'clm' table.
 */
static void
push_secrets(lua_State *L, const char *config_path)
{
	autofree char *spath = sibling_path(config_path, "secrets.lua");
	if (spath == NULL) {
		lua_newtable(L);
		return;
	}

	struct stat st;
	if (stat(spath, &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
		clm_debug("secrets: %s is readable by group/other; "
		    "consider chmod 600", spath);
	}

	if (luaL_loadfile(L, spath) != LUA_OK) {
		clm_debug("secrets: %s: %s", spath, lua_tostring(L, -1));
		lua_pop(L, 1);
		lua_newtable(L);
		return;
	}
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		clm_debug("secrets: error in %s: %s", spath,
		    lua_tostring(L, -1));
		lua_pop(L, 1);
		lua_newtable(L);
		return;
	}
	if (!lua_istable(L, -1)) {
		clm_debug("secrets: %s did not return a table", spath);
		lua_pop(L, 1);
		lua_newtable(L);
	}
}

CLM_API struct clm_lua_cfg *
clm_lua_cfg_load(const char *path)
{
	struct clm_lua_cfg *cfg;
	lua_State *L;

	if (path == NULL)
		return NULL;

	L = luaL_newstate();
	if (L == NULL)
		return NULL;

	luaL_requiref(L, "_G", luaopen_base, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "string", luaopen_string, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "table", luaopen_table, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "math", luaopen_math, 1);
	lua_pop(L, 1);
	clm_lua_json_open(L);

	/* clm.secrets: visible to config.lua and, since agent profile files
	 * share this same lua_State, to them too. */
	lua_newtable(L); /* clm */
	push_secrets(L, path); /* clm, secrets */
	lua_setfield(L, -2, "secrets"); /* clm.secrets = secrets */
	lua_setglobal(L, "clm"); /* clm = {secrets = ...} */

	if (luaL_loadfile(L, path) != LUA_OK) {
		clm_debug("config: failed to load %s: %s", path,
		    lua_tostring(L, -1));
		lua_close(L);
		return NULL;
	}
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		clm_debug("config: error in %s: %s", path,
		    lua_tostring(L, -1));
		lua_close(L);
		return NULL;
	}
	if (!lua_istable(L, -1)) {
		clm_debug("config: %s did not return a table", path);
		lua_close(L);
		return NULL;
	}

	cfg = calloc(1, sizeof(*cfg));
	if (cfg == NULL) {
		lua_close(L);
		return NULL;
	}
	cfg->L = L;
	cfg->cfg_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	cfg->agent_ref = LUA_NOREF;
	return cfg;
}

CLM_API int
clm_lua_cfg_load_agent(struct clm_lua_cfg *cfg, const char *agents_dir,
    const char *agent_name)
{
	lua_State *L = cfg->L;
	char *aname = NULL;

	/* If no name given, read config.agent. */
	if (agent_name == NULL) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
		lua_getfield(L, -1, "agent");
		if (lua_isstring(L, -1))
			agent_name = lua_tostring(L, -1);
		lua_pop(L, 2);
		if (agent_name == NULL)
			return -1;
	}

	/* Check if config.agents[name] exists (inline). */
	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, "agents");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, agent_name);
		if (lua_istable(L, -1)) {
			if (cfg->agent_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, cfg->agent_ref);
			cfg->agent_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pop(L, 2); /* agents table, config table */
			free(cfg->resolved_agent_name);
			cfg->resolved_agent_name = strdup(agent_name);
			return 0;
		}
		lua_pop(L, 1); /* nil */
	}
	lua_pop(L, 2); /* agents (or nil), config table */

	/* Try loading agents_dir/<name>.lua */
	if (agents_dir == NULL)
		return -1;

	size_t dlen = strlen(agents_dir);
	size_t nlen = strlen(agent_name);
	aname = malloc(dlen + 1 + nlen + 4 + 1);
	if (aname == NULL)
		return -1;
	(void)snprintf(aname, dlen + 1 + nlen + 5, "%s/%s.lua",
	    agents_dir, agent_name);

	if (luaL_loadfile(L, aname) != LUA_OK) {
		clm_debug("config: agent file %s: %s", aname,
		    lua_tostring(L, -1));
		lua_pop(L, 1);
		free(aname);
		return -1;
	}
	free(aname);

	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		clm_debug("config: agent error: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	if (cfg->agent_ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, cfg->agent_ref);
	cfg->agent_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	free(cfg->resolved_agent_name);
	cfg->resolved_agent_name = strdup(agent_name);
	return 0;
}

CLM_API const char *
clm_lua_cfg_get_str(struct clm_lua_cfg *cfg, const char *key)
{
	lua_State *L = cfg->L;
	const char *val = NULL;

	/* Check agent table first, then top-level config. */
	if (cfg->agent_ref != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->agent_ref);
		lua_getfield(L, -1, key);
		if (lua_isstring(L, -1))
			val = lua_tostring(L, -1);
		lua_pop(L, 2);
		if (val != NULL)
			return val;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, key);
	if (lua_isstring(L, -1))
		val = lua_tostring(L, -1);
	lua_pop(L, 2);
	return val;
}

/*
 * Collect string entries from the array table at the top of the stack
 * into a malloc'd NULL-terminated list. Pops nothing; returns NULL if
 * the value is not a table or contains no strings.
 */
static char **
collect_str_list(lua_State *L)
{
	autofreev char **list = NULL;
	char **ret;
	size_t n, count = 0, kept = 0;

	if (!lua_istable(L, -1))
		return NULL;

	n = lua_rawlen(L, -1);
	if (n == 0)
		return NULL;

	list = calloc(n + 1, sizeof(*list));
	if (list == NULL)
		return NULL;

	for (count = 1; count <= n; count++) {
		lua_rawgeti(L, -1, (lua_Integer)count);
		/* lua_tostring would coerce numbers in place, corrupting the
		 * table during iteration -- only accept real strings. */
		if (lua_type(L, -1) == LUA_TSTRING) {
			list[kept] = strdup(lua_tostring(L, -1));
			if (list[kept] == NULL) {
				lua_pop(L, 1);
				return NULL;
			}
			kept++;
		}
		lua_pop(L, 1);
	}

	if (kept == 0)
		return NULL;

	ret = list;
	list = NULL;
	return ret;
}

CLM_API char **
clm_lua_cfg_get_str_list(struct clm_lua_cfg *cfg, const char *key)
{
	lua_State *L = cfg->L;
	char **list = NULL;

	/* Agent table first, then top-level config: same precedence as
	 * clm_lua_cfg_get_str. */
	if (cfg->agent_ref != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->agent_ref);
		lua_getfield(L, -1, key);
		list = collect_str_list(L);
		lua_pop(L, 2);
		if (list != NULL)
			return list;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, key);
	list = collect_str_list(L);
	lua_pop(L, 2);
	return list;
}

CLM_API void
clm_lua_cfg_free_str_list(char **list)
{
	char **p;

	if (list == NULL)
		return;
	for (p = list; *p != NULL; p++)
		free(*p);
	free(list);
}

/*
 * Shared lookup for clm_lua_cfg_{provider,model}_{str,int}: reads
 * config.<table>[entry_name][key] off the config table at the top of the
 * registry ref. Leaves the stack as it found it. Returns NULL/false if the
 * table, entry, or key is missing or of the wrong shape.
 */
static bool
cfg_table_entry_str(lua_State *L, int cfg_ref, const char *table,
    const char *entry_name, const char *key, const char **out)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg_ref);
	lua_getfield(L, -1, table);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return false;
	}
	lua_getfield(L, -1, entry_name);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 3);
		return false;
	}
	lua_getfield(L, -1, key);
	if (lua_isstring(L, -1))
		*out = lua_tostring(L, -1);
	lua_pop(L, 4);
	return true;
}

static bool
cfg_table_entry_int(lua_State *L, int cfg_ref, const char *table,
    const char *entry_name, const char *key, int64_t *out)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg_ref);
	lua_getfield(L, -1, table);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return false;
	}
	lua_getfield(L, -1, entry_name);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 3);
		return false;
	}
	lua_getfield(L, -1, key);
	if (lua_isnumber(L, -1))
		*out = (int64_t)lua_tonumber(L, -1);
	lua_pop(L, 4);
	return true;
}

CLM_API const char *
clm_lua_cfg_provider_str(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *key)
{
	const char *val = NULL;
	cfg_table_entry_str(cfg->L, cfg->cfg_ref, "providers", provider_name,
	    key, &val);
	return val;
}

CLM_API int64_t
clm_lua_cfg_provider_int(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *key, int64_t fallback)
{
	int64_t val = fallback;
	cfg_table_entry_int(cfg->L, cfg->cfg_ref, "providers", provider_name,
	    key, &val);
	return val;
}

/*
 * Model entries live nested under their owning provider
 * (config.providers[X].models[model_id]), keyed by the literal wire model
 * id -- see clm-config(5). A model is therefore always addressed as a
 * "provider/model-id" pair (split by src/model_spec.h's
 * split_provider_model() at every call site that takes a model spec:
 * config.lua's top-level `model`, -m/--model, /model), never looked up by
 * name alone, so there is no ambiguity to search through the way an
 * unaddressed name would need.
 *
 * Read config.providers[provider_name].models[model_id][key]. Leaves the
 * stack as it found it (lua_gettop/lua_settop bracket the whole function,
 * so an OOM or shape mismatch at any depth can't leave it unbalanced).
 */
static bool
cfg_provider_model_str(lua_State *L, int cfg_ref, const char *provider_name,
    const char *model_id, const char *key, const char **out)
{
	int base = lua_gettop(L);
	bool found = false;

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg_ref);
	lua_getfield(L, -1, "providers");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, provider_name);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "models");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, model_id);
				if (lua_istable(L, -1)) {
					lua_getfield(L, -1, key);
					if (lua_isstring(L, -1))
						*out = lua_tostring(L, -1);
					found = true;
				}
			}
		}
	}
	lua_settop(L, base);
	return found;
}

static bool
cfg_provider_model_int(lua_State *L, int cfg_ref, const char *provider_name,
    const char *model_id, const char *key, int64_t *out)
{
	int base = lua_gettop(L);
	bool found = false;

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg_ref);
	lua_getfield(L, -1, "providers");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, provider_name);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "models");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, model_id);
				if (lua_istable(L, -1)) {
					lua_getfield(L, -1, key);
					if (lua_isnumber(L, -1))
						*out = (int64_t)lua_tonumber(L, -1);
					found = true;
				}
			}
		}
	}
	lua_settop(L, base);
	return found;
}

CLM_API const char *
clm_lua_cfg_provider_model_str(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *model_id, const char *key)
{
	const char *val = NULL;
	cfg_provider_model_str(cfg->L, cfg->cfg_ref, provider_name, model_id,
	    key, &val);
	return val;
}

CLM_API int64_t
clm_lua_cfg_provider_model_int(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *model_id, const char *key,
    int64_t fallback)
{
	int64_t val = fallback;
	cfg_provider_model_int(cfg->L, cfg->cfg_ref, provider_name, model_id,
	    key, &val);
	return val;
}

/*
 * Collect every config.providers[*].models entry into one flat, malloc'd
 * NULL-terminated list of "provider/model-id" compound strings -- the
 * "models" case of clm_lua_cfg_list_names, matching the addressed spec
 * format every model-taking call site expects (see split_provider_model()
 * in src/model_spec.h). Formatted here rather than left as a bare model id
 * since a bare id doesn't uniquely address anything once nested under more
 * than one provider.
 */
static char **
list_all_model_names(lua_State *L, int cfg_ref)
{
	int base = lua_gettop(L);
	autofreev char **list = NULL;
	size_t cap = 0, n = 0;
	char **ret;

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg_ref);
	lua_getfield(L, -1, "providers");
	if (!lua_istable(L, -1)) {
		lua_settop(L, base);
		return NULL;
	}

	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		/* provider name at -2, provider table at -1 */
		if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
			const char *pname = lua_tostring(L, -2);

			lua_getfield(L, -1, "models");
			if (lua_istable(L, -1)) {
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					/* model id at -2, model table at -1 */
					if (lua_type(L, -2) == LUA_TSTRING) {
						const char *mid = lua_tostring(L, -2);
						size_t need;
						char *spec;

						if (n + 1 >= cap) {
							size_t newcap = cap ? cap * 2 : 8;
							char **grown = realloc(list,
							    (newcap + 1) * sizeof(*grown));
							if (grown == NULL) {
								lua_settop(L, base);
								return NULL;
							}
							list = grown;
							cap = newcap;
						}
						need = strlen(pname) + 1 /* '/' */
						    + strlen(mid) + 1 /* NUL */;
						spec = malloc(need);
						if (spec == NULL) {
							lua_settop(L, base);
							return NULL;
						}
						(void)snprintf(spec, need, "%s/%s",
						    pname, mid);
						list[n++] = spec;
						list[n] = NULL;
					}
					lua_pop(L, 1); /* model table; keep key */
				}
			}
			lua_pop(L, 1); /* models table or nil */
		}
		lua_pop(L, 1); /* provider table; keep key for lua_next */
	}
	lua_settop(L, base);

	if (n == 0)
		return NULL;

	ret = list;
	list = NULL;
	return ret;
}

CLM_API char **
clm_lua_cfg_list_names(struct clm_lua_cfg *cfg, const char *table)
{
	lua_State *L = cfg->L;
	autofreev char **list = NULL;
	size_t cap = 0, n = 0;
	char **ret;

	/* "models" no longer names a top-level config table -- entries live
	 * nested under their owning provider (config.providers[*].models),
	 * see the comment above list_all_model_names(). Every other table
	 * this is called with ("providers") is still top-level, so fall
	 * through to the plain walk below for anything else. */
	if (strcmp(table, "models") == 0)
		return list_all_model_names(L, cfg->cfg_ref);

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, table);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return NULL;
	}

	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		/* key at -2, value at -1. Only string keys are named entries
		 * (providers/models tables are keyed by name, never by
		 * array index), so this also naturally skips any stray
		 * array-part elements. */
		if (lua_type(L, -2) == LUA_TSTRING) {
			char *name;

			if (n + 1 >= cap) {
				size_t newcap = cap ? cap * 2 : 8;
				char **grown = realloc(list,
				    (newcap + 1) * sizeof(*grown));
				if (grown == NULL) {
					lua_pop(L, 2); /* value, key */
					lua_pop(L, 2); /* table, config */
					return NULL;
				}
				list = grown;
				cap = newcap;
			}
			name = strdup(lua_tostring(L, -2));
			if (name == NULL) {
				lua_pop(L, 2); /* value, key */
				lua_pop(L, 2); /* table, config */
				return NULL;
			}
			list[n++] = name;
			list[n] = NULL;
		}
		lua_pop(L, 1); /* pop value, keep key for lua_next */
	}
	lua_pop(L, 2); /* table, config */

	if (n == 0)
		return NULL;

	ret = list;
	list = NULL;
	return ret;
}

CLM_API char *
clm_lua_cfg_tools_json(struct clm_lua_cfg *cfg)
{
	lua_State *L = cfg->L;
	char *out;

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, "tools");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return NULL;
	}
	out = lua_table_to_json(L, -1);
	lua_pop(L, 2);
	return out;
}

/*
 * Get the mcp_servers config as a JSON string, e.g.:
 *   mcp_servers = {
 *     { name = "fs", transport = "stdio", command = {"mcp-server-fs", "/tmp"} },
 *     { name = "search", transport = "http", url = "https://.../mcp", api_key = "..." },
 *   }
 * Caller owns the returned string (free with free()). NULL if unset.
 */
CLM_API char *
clm_lua_cfg_mcp_servers_json(struct clm_lua_cfg *cfg)
{
	lua_State *L = cfg->L;
	char *out;

	lua_rawgeti(L, LUA_REGISTRYINDEX, cfg->cfg_ref);
	lua_getfield(L, -1, "mcp_servers");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return NULL;
	}
	out = lua_table_to_json(L, -1);
	lua_pop(L, 2);
	return out;
}

CLM_API void
clm_lua_cfg_free(struct clm_lua_cfg *cfg)
{
	if (cfg == NULL)
		return;
	if (cfg->L != NULL)
		lua_close(cfg->L);
	free(cfg->resolved_agent_name);
	free(cfg);
}

/*
 * Returns the agent name actually resolved by the most recent successful
 * clm_lua_cfg_load_agent() call (whether that came from an explicit
 * agent_name argument or a fallback read of config.agent), or NULL if
 * clm_lua_cfg_load_agent() has not yet succeeded. Distinct from reading
 * the "agent" field directly off the config table, which only ever
 * reflects config.lua's static default and ignores any -a/--agent
 * override actually used to select the loaded agent.
 */
CLM_API const char *
clm_lua_cfg_get_agent_name(struct clm_lua_cfg *cfg)
{
	if (cfg == NULL)
		return NULL;
	return cfg->resolved_agent_name;
}

/* Legacy wrapper. */
CLM_API char *
clm_lua_load_config(const char *path)
{
	struct clm_lua_cfg *cfg = clm_lua_cfg_load(path);
	if (cfg == NULL)
		return NULL;
	char *json = clm_lua_cfg_tools_json(cfg);
	clm_lua_cfg_free(cfg);
	return json;
}
