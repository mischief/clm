// SPDX-License-Identifier: ISC
/*
 * Lua HTTP bindings: http.get(url) and http.post(url, body)
 * These yield the calling coroutine and resume it with the response
 * when the async HTTP request completes.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/host.h"
#include "clm/http.h"
#include "clm/log.h"
#include "lua_internal.h"
#include "useful.h"
#include "banned.h"

/* Prototype — called from lua_plugin.c. */
int clm_lua_http_open(lua_State *L, struct clm_agent *agent);

/* Forward declarations from lua_plugin.c for budget tracking. */
struct clm_lua_plugin;
void clm_lua_http_done(struct clm_lua_plugin *plugin);
int clm_lua_http_start(struct clm_lua_plugin *plugin);
void clm_lua_budget_report(struct clm_lua_plugin *plugin, const char *name);

/* Invocation-thread guard (lua_plugin.c): the async HTTP model requires that
 * http.get/post run directly on the invocation coroutine so their lua_yield
 * unwinds to clm's C-level lua_resume. */
int clm_lua_is_invocation_thread(lua_State *L);
void clm_lua_mark_invocation_thread(lua_State *L, lua_State *co, int on);
void clm_lua_clear_invocation_registry(lua_State *L);
int clm_lua_resume_with_deadline(struct clm_lua_plugin *plugin, lua_State *co,
    lua_State *from, int nargs, int *nresults, uint64_t timeout_ms);

enum lua_http_start_result {
	LUA_HTTP_START_PENDING,
	LUA_HTTP_START_SUCCESS,
	LUA_HTTP_START_ERROR,
};

/* State for one in-flight Lua HTTP request. */
struct lua_http_req {
	struct clm_lua_pending pending;
	lua_State *co;              /* the yielded coroutine */
	lua_State *main_L;          /* plugin's main state */
	int co_ref;                 /* registry ref for the coroutine */
	struct clm_agent *agent;
	struct clm_http_call *req; /* the underlying async request */
	char *tool_name;
	struct clm_tool_invocation *inv; /* for failing the tool on error */
	int starting;
	enum lua_http_start_result start_result;
	int start_status;
	char *start_body;
	char start_error[256];
};

static void
lua_http_req_free(struct lua_http_req *lr)
{
	free(lr->tool_name);
	free(lr);
}

static void
lua_http_teardown(struct clm_lua_pending *pending)
{
	struct lua_http_req *lr = (struct lua_http_req *)pending;
	struct clm_tool_invocation *inv = lr->inv;

	if (inv != NULL)
		clm_tool_invocation_set_cancel(inv, NULL, NULL);
	lr->inv = NULL;
	lr->co = NULL;
	lr->main_L = NULL;
	if (lr->agent != NULL && lr->agent->host != NULL &&
	    lr->agent->host->http_cancel != NULL && lr->req != NULL)
		lr->agent->host->http_cancel(lr->req);
	if (inv != NULL)
		clm_tool_fail(inv, "lua plugin environment is shutting down");
}

/*
 * Tool timeout while an http.get/post is still in flight: abort the
 * underlying transfer so its completion callback fires now, while `inv`
 * is still alive, instead of arriving later against a freed invocation
 * (the batch/invocation are freed as soon as the timeout path finalizes
 * the tool result — see on_timeout()/inv_finalize() in tools.c).
 */
static void
lua_http_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct lua_http_req *lr = user;
	(void)inv;
	if (lr->agent != NULL && lr->agent->host != NULL &&
	    lr->agent->host->http_cancel != NULL && lr->req != NULL)
		lr->agent->host->http_cancel(lr->req);
}

/* ------------------------------------------------------------------ */
/* HTTP completion callbacks                                            */
/* ------------------------------------------------------------------ */

/*
 * Resume the Lua coroutine with a response table:
 *   { status = 200, body = "..." }
 */
static void
lua_http_on_success(struct clm_http_response *resp, void *user)
{
	struct lua_http_req *lr = user;
	struct clm_lua_plugin *plugin;
	lua_State *co;
	lua_State *L;
	int nres, rc;

	if (lr->starting) {
		lr->start_result = LUA_HTTP_START_SUCCESS;
		lr->start_status = resp->status_code;
		lr->start_body = resp->body;
		resp->body = NULL;
		return;
	}

	plugin = clm_lua_pending_remove(&lr->pending);
	if (plugin == NULL) {
		free(resp->body);
		resp->body = NULL;
		lua_http_req_free(lr);
		return;
	}
	co = lr->co;
	L = lr->main_L;
	if (lr->inv != NULL)
		clm_tool_invocation_set_cancel(lr->inv, NULL, NULL);

	/* Guard: if the coroutine is no longer suspended (already finished
	 * or errored out via another path), resuming it would crash in
	 * lua's unroll() with a NULL ci.  Clean up and bail. */
	if (lua_status(co) != LUA_YIELD) {
		clm_debug("lua http: coroutine not yielded (status=%d), "
		    "skipping resume", lua_status(co));
		free(resp->body);
		resp->body = NULL;
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return;
	}

	/* Second guard: verify the coroutine's internal state is sane.
	 * A yielded coroutine with lua_gettop < 0 or absurdly high means
	 * the state was corrupted (e.g. by a concurrent error on a shared
	 * main state). Bail rather than crash in lua_newtable. */
	int top = lua_gettop(co);
	if (top < 0 || top > 500) {
		clm_debug("lua http: coroutine stack corrupt (top=%d), "
		    "skipping resume", top);
		free(resp->body);
		resp->body = NULL;
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return;
	}

	/* Third guard: verify co_ref still resolves to the same coroutine.
	 * If the ref was freed and reused, it could point to a different
	 * object, meaning our lr->co is stale. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, lr->co_ref);
	lua_State *ref_co = lua_tothread(L, -1);
	lua_pop(L, 1);
	if (ref_co != co) {
		clm_debug("lua http: co_ref %d no longer points to original "
		    "coroutine (got %p, expected %p), skipping resume",
		    lr->co_ref, (void *)ref_co, (void *)co);
		free(resp->body);
		resp->body = NULL;
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return;
	}

	/* Push result table onto coroutine stack. */
	lua_newtable(co);
	lua_pushinteger(co, resp->status_code);
	lua_setfield(co, -2, "status");
	if (resp->body != NULL) {
		lua_pushstring(co, resp->body);
		lua_setfield(co, -2, "body");
	}

	/* Free the response body (we've copied it to Lua). */
	free(resp->body);
	resp->body = NULL;

	/* Resume the coroutine with one result (the table). */
	nres = 0;
	rc = clm_lua_resume_with_deadline(plugin, co, L, 1, &nres,
	    clm_tool_invocation_timeout_ms(lr->inv));
	if (rc == LUA_OK) {
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_budget_report(plugin, lr->tool_name ? lr->tool_name : "?");
	} else if (rc == LUA_YIELD) {
		/* Yielded again (e.g. another HTTP request in the same tool).
		 * The next HTTP callback will resume it. */
	} else {
		/* Runtime error after resume. Fail the tool so it doesn't hang. */
		const char *err = lua_tostring(co, -1);
		clm_debug("lua http: coroutine error on resume: %s",
		    err ? err : "(unknown)");
		if (lr->inv != NULL)
			clm_tool_fail(lr->inv, err ? err : "Lua runtime error");
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_budget_report(plugin, lr->tool_name ? lr->tool_name : "?");
	}

	clm_lua_http_done(plugin);
	lua_http_req_free(lr);
}

/*
 * Resume the Lua coroutine with nil, error_message.
 */
static void
lua_http_on_error(int error_code, const char *error_msg, void *user)
{
	struct lua_http_req *lr = user;
	struct clm_lua_plugin *plugin;
	lua_State *co;
	lua_State *L;
	int nres, rc;

	if (lr->starting) {
		lr->start_result = LUA_HTTP_START_ERROR;
		(void)snprintf(lr->start_error, sizeof(lr->start_error), "%s",
		    error_msg ? error_msg : "unknown HTTP error");
		return;
	}

	plugin = clm_lua_pending_remove(&lr->pending);
	if (plugin == NULL) {
		lua_http_req_free(lr);
		return;
	}
	co = lr->co;
	L = lr->main_L;
	if (lr->inv != NULL)
		clm_tool_invocation_set_cancel(lr->inv, NULL, NULL);

	/* Guard: coroutine already finished, don't resume. */
	if (lua_status(co) != LUA_YIELD) {
		clm_debug("lua http error: coroutine not yielded (status=%d), "
		    "skipping resume", lua_status(co));
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return;
	}

	/* Push nil + error string. */
	lua_pushnil(co);
	lua_pushstring(co, error_msg ? error_msg : "unknown HTTP error");

	nres = 0;
	rc = clm_lua_resume_with_deadline(plugin, co, L, 2, &nres,
	    clm_tool_invocation_timeout_ms(lr->inv));
	if (rc == LUA_OK) {
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_budget_report(plugin, lr->tool_name ? lr->tool_name : "?");
	} else if (rc == LUA_YIELD) {
		/* Yielded again. */
	} else {
		const char *err = lua_tostring(co, -1);
		clm_debug("lua http: coroutine error on error resume: %s",
		    err ? err : "(unknown)");
		if (lr->inv != NULL)
			clm_tool_fail(lr->inv, err ? err : "Lua runtime error");
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_clear_invocation_registry(L);
		clm_lua_budget_report(plugin, lr->tool_name ? lr->tool_name : "?");
	}

	(void)error_code;
	clm_lua_http_done(plugin);
	lua_http_req_free(lr);
}

static int
lua_http_return_inline(lua_State *L, struct lua_http_req *lr)
{
	struct clm_lua_plugin *plugin;
	int nres;

	plugin = clm_lua_pending_remove(&lr->pending);
	if (lr->start_result == LUA_HTTP_START_SUCCESS) {
		lua_newtable(L);
		lua_pushinteger(L, lr->start_status);
		lua_setfield(L, -2, "status");
		if (lr->start_body != NULL) {
			lua_pushstring(L, lr->start_body);
			lua_setfield(L, -2, "body");
		}
		free(lr->start_body);
		nres = 1;
	} else {
		lua_pushnil(L);
		lua_pushstring(L, lr->start_error);
		nres = 2;
	}

	clm_lua_http_done(plugin);
	lua_http_req_free(lr);
	return nres;
}

/* ------------------------------------------------------------------ */
/* Lua-callable HTTP functions (yield the coroutine)                    */
/* ------------------------------------------------------------------ */

/*
 * ctx:http_get(url [, headers_table])
 * Yields. On resume returns: response_table or nil, err.
 */
/* Free a NULL-terminated header array from lua_collect_headers. */
static void
free_headers(char **h)
{
	if (h == NULL)
		return;
	for (size_t i = 0; h[i] != NULL; i++)
		free(h[i]);
	free(h);
}

/*
 * Collect "Name: Value" strings from the string-keyed Lua table at `idx` (a
 * non-table is treated as empty) into a fresh NULL-terminated array. `seed`, if
 * non-NULL, becomes the first entry (e.g. a default Content-Type). The strings
 * are copied, so the result outlives the Lua stack. Returns NULL on OOM; the
 * caller frees with free_headers.
 */
static char **
lua_collect_headers(lua_State *L, int idx, const char *seed)
{
	size_t cap = 8, n = 0;
	char **arr = malloc(cap * sizeof(*arr));
	if (arr == NULL)
		return NULL;
	if (seed != NULL) {
		if ((arr[n] = strdup(seed)) == NULL)
			goto oom;
		n++;
	}
	if (lua_istable(L, idx)) {
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING &&
			    lua_type(L, -1) == LUA_TSTRING) {
				char hdr[512];
				(void)snprintf(hdr, sizeof(hdr), "%s: %s",
				    lua_tostring(L, -2), lua_tostring(L, -1));
				if (n + 1 >= cap) {
					size_t ncap = cap * 2;
					char **na = realloc(arr, ncap * sizeof(*arr));
					if (na == NULL) {
						lua_pop(L, 2);
						goto oom;
					}
					arr = na;
					cap = ncap;
				}
				if ((arr[n] = strdup(hdr)) == NULL) {
					lua_pop(L, 2);
					goto oom;
				}
				n++;
			}
			lua_pop(L, 1);
		}
	}
	arr[n] = NULL;
	return arr;
oom:
	arr[n] = NULL;
	free_headers(arr);
	return NULL;
}

static int
lua_ctx_http_get(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	struct lua_http_req *lr;
	struct clm_lua_plugin *plugin;
	int r;

	/* Must run directly on the invocation coroutine: a nested coroutine or a
	 * load-time (main-thread) call would yield to the wrong lua_resume and
	 * desync completion tracking. Reject it before starting any request. */
	if (!clm_lua_is_invocation_thread(L)) {
		clm_debug("lua http: http.get rejected — not called from a tool "
		    "invocation coroutine (nested coroutine or load time)");
		return luaL_error(L, "http.get may only be called directly from a "
		    "tool invocation, not from a nested coroutine or at load time");
	}

	/* Retrieve the agent from the registry (set during http module open). */
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_agent");
	struct clm_agent *agent = lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (agent == NULL)
		return luaL_error(L, "http_get: no agent context");

	if (agent->host == NULL || agent->host->http_post == NULL)
		return luaL_error(L, "http_get: no HTTP transport");

	lr = calloc(1, sizeof(*lr));
	if (lr == NULL)
		return luaL_error(L, "http_get: out of memory");

	lr->co = L;
	lr->agent = agent;

	/* Get the main state, coroutine ref, and plugin from registry. */
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_main_L");
	lr->main_L = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_co_ref");
	lr->co_ref = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_plugin");
	plugin = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_tool_name");
	const char *tool_name = lua_tostring(L, -1);
	lr->tool_name = tool_name != NULL ? strdup(tool_name) : NULL;
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_inv");
	lr->inv = lua_touserdata(L, -1);
	lua_pop(L, 1);

	/* Enforce HTTP budget. */
	{
		int br = clm_lua_http_start(plugin);
		if (br == -1) {
			lua_http_req_free(lr);
			return luaL_error(L, "http_get: too many in-flight requests");
		}
		if (br == -2) {
			lua_http_req_free(lr);
			return luaL_error(L, "http_get: request limit exceeded");
		}
	}

	/* Build optional request headers from arg 2 (table). */
	char **hdrs = lua_collect_headers(L, 2, NULL);
	if (hdrs == NULL) {
		lua_http_req_free(lr);
		return luaL_error(L, "http_get: out of memory");
	}

	struct clm_http_req hreq = {
		.url = url,
		.api_key = NULL,
		.body = NULL,
		.headers = (const char *const *)hdrs,
		.client_suffix = lr->tool_name,
	};
	r = clm_lua_pending_add(plugin, &lr->pending, lua_http_teardown);
	if (r < 0) {
		free_headers(hdrs);
		lua_http_req_free(lr);
		return luaL_error(L, "http_get: plugin is shutting down");
	}
	lr->starting = 1;
	r = agent->host->http_post(agent->host->ctx, &hreq, lua_http_on_success,
	    lua_http_on_error, NULL, lr, &lr->req);
	lr->starting = 0;
	free_headers(hdrs);
	if (r < 0) {
		(void)clm_lua_pending_remove(&lr->pending);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_get: failed to start request: %s",
		    strerror(-r));
	}
	if (lr->start_result != LUA_HTTP_START_PENDING)
		return lua_http_return_inline(L, lr);

	/* If the tool times out while this request is still in flight, abort
	 * it instead of letting it complete later against a freed inv. */
	if (lr->inv != NULL)
		clm_tool_invocation_set_cancel(lr->inv, lua_http_cancel, lr);

	/* Yield the coroutine. The HTTP callbacks will resume it. */
	return lua_yield(L, 0);
}

/*
 * ctx:http_post(url, body [, content_type])
 * Yields. On resume returns: response_table or nil, err.
 */
static int
lua_ctx_http_post(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checkstring(L, 2);
	struct lua_http_req *lr;
	struct clm_lua_plugin *plugin;
	int r;

	/* See lua_ctx_http_get: must run on the invocation coroutine. */
	if (!clm_lua_is_invocation_thread(L)) {
		clm_debug("lua http: http.post rejected — not called from a tool "
		    "invocation coroutine (nested coroutine or load time)");
		return luaL_error(L, "http.post may only be called directly from a "
		    "tool invocation, not from a nested coroutine or at load time");
	}

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_agent");
	struct clm_agent *agent = lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (agent == NULL)
		return luaL_error(L, "http_post: no agent context");

	if (agent->host == NULL || agent->host->http_post == NULL)
		return luaL_error(L, "http_post: no HTTP transport");

	lr = calloc(1, sizeof(*lr));
	if (lr == NULL)
		return luaL_error(L, "http_post: out of memory");

	lr->co = L;
	lr->agent = agent;

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_main_L");
	lr->main_L = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_co_ref");
	lr->co_ref = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_plugin");
	plugin = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_tool_name");
	const char *tool_name = lua_tostring(L, -1);
	lr->tool_name = tool_name != NULL ? strdup(tool_name) : NULL;
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_inv");
	lr->inv = lua_touserdata(L, -1);
	lua_pop(L, 1);

	/* Enforce HTTP budget. */
	{
		int br = clm_lua_http_start(plugin);
		if (br == -1) {
			lua_http_req_free(lr);
			return luaL_error(L, "http_post: too many in-flight requests");
		}
		if (br == -2) {
			lua_http_req_free(lr);
			return luaL_error(L, "http_post: request limit exceeded");
		}
	}

	/* Build request headers. Always set Content-Type for POST; arg 3, if a
	 * table, contributes extra headers. */
	char **hdrs = lua_collect_headers(L, 3, "Content-Type: application/json");
	if (hdrs == NULL) {
		lua_http_req_free(lr);
		return luaL_error(L, "http_post: out of memory");
	}

	struct clm_http_req hreq = {
		.url = url,
		.api_key = NULL,
		.body = body,
		.headers = (const char *const *)hdrs,
		.client_suffix = lr->tool_name,
	};
	r = clm_lua_pending_add(plugin, &lr->pending, lua_http_teardown);
	if (r < 0) {
		free_headers(hdrs);
		lua_http_req_free(lr);
		return luaL_error(L, "http_post: plugin is shutting down");
	}
	lr->starting = 1;
	r = agent->host->http_post(agent->host->ctx, &hreq, lua_http_on_success,
	    lua_http_on_error, NULL, lr, &lr->req);
	lr->starting = 0;
	free_headers(hdrs);
	if (r < 0) {
		(void)clm_lua_pending_remove(&lr->pending);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_post: failed to start request: %s",
		    strerror(-r));
	}
	if (lr->start_result != LUA_HTTP_START_PENDING)
		return lua_http_return_inline(L, lr);

	if (lr->inv != NULL)
		clm_tool_invocation_set_cancel(lr->inv, lua_http_cancel, lr);

	return lua_yield(L, 0);
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */

int
clm_lua_http_open(lua_State *L, struct clm_agent *agent)
{
	/* Store the agent pointer in the registry for retrieval from within
	 * coroutines; the HTTP transport is reached via agent->host. */
	lua_pushlightuserdata(L, agent);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_agent");

	/* Store main L for coroutine resume. */
	lua_pushlightuserdata(L, L);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_main_L");

	/* Register http functions as globals (callable from coroutines). */
	lua_newtable(L);
	lua_pushcfunction(L, lua_ctx_http_get);
	lua_setfield(L, -2, "get");
	lua_pushcfunction(L, lua_ctx_http_post);
	lua_setfield(L, -2, "post");
	lua_setglobal(L, "http");

	return 0;
}
