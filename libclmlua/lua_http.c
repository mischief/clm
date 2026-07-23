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
	struct clm_lua_coroutine *lco;
	struct clm_agent *agent;
	struct clm_http_call *req;
	int starting;
	enum lua_http_start_result start_result;
	int start_status;
	char *start_body;
	char start_error[256];
};

static void
lua_http_req_free(struct lua_http_req *lr)
{
	free(lr);
}

static void
lua_http_teardown(struct clm_lua_pending *pending)
{
	struct lua_http_req *lr = (struct lua_http_req *)pending;
	struct clm_http_call *req = lr->req;
	struct clm_agent *agent = lr->agent;

	if (lr->lco != NULL)
		clm_lua_http_done(lr->lco->plugin);
	lr->lco = NULL;
	lr->req = NULL;
	if (agent != NULL && agent->host != NULL &&
	    agent->host->http_cancel != NULL && req != NULL)
		agent->host->http_cancel(req);
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
	struct clm_lua_coroutine *lco;
	struct clm_lua_plugin *plugin;
	lua_State *co;
	int nres = 0, rc;

	if (lr->starting) {
		lr->start_result = LUA_HTTP_START_SUCCESS;
		lr->start_status = resp->status_code;
		lr->start_body = resp->body;
		resp->body = NULL;
		return;
	}
	lco = lr->pending.coroutine;
	plugin = clm_lua_pending_remove(&lr->pending);
	if (plugin == NULL || lco == NULL) {
		free(resp->body);
		resp->body = NULL;
		lua_http_req_free(lr);
		return;
	}
	co = lco->co;
	if (lua_status(co) != LUA_YIELD) {
		clm_debug("lua http: coroutine not yielded (status=%d)",
		    lua_status(co));
		free(resp->body);
		resp->body = NULL;
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		clm_lua_coroutine_finish(lco, LUA_ERRRUN, 0,
		    "lua http coroutine was not suspended");
		return;
	}
	int top = lua_gettop(co);
	if (top < 0 || top > 500) {
		free(resp->body);
		resp->body = NULL;
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		clm_lua_coroutine_finish(lco, LUA_ERRRUN, 0,
		    "lua http coroutine stack is corrupt");
		return;
	}
	lua_rawgeti(lco->main_L, LUA_REGISTRYINDEX, lco->ref);
	lua_State *ref_co = lua_tothread(lco->main_L, -1);
	lua_pop(lco->main_L, 1);
	if (ref_co != co) {
		free(resp->body);
		resp->body = NULL;
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		clm_lua_coroutine_finish(lco, LUA_ERRRUN, 0,
		    "lua http coroutine reference is stale");
		return;
	}

	lua_newtable(co);
	lua_pushinteger(co, resp->status_code);
	lua_setfield(co, -2, "status");
	if (resp->body != NULL) {
		lua_pushstring(co, resp->body);
		lua_setfield(co, -2, "body");
	}
	free(resp->body);
	resp->body = NULL;
	uint64_t timeout = clm_tool_invocation_timeout_ms(lco->call->inv);
	rc = clm_lua_resume_with_deadline(plugin, co, lco->main_L, 1, &nres,
	    timeout);
	const char *err = rc == LUA_OK || rc == LUA_YIELD ? NULL :
	    lua_tostring(co, -1);
	if (rc != LUA_OK && rc != LUA_YIELD)
		clm_debug("lua http: coroutine error on resume: %s",
		    err ? err : "(unknown)");
	clm_lua_http_done(plugin);
	lua_http_req_free(lr);
	if (rc != LUA_YIELD)
		clm_lua_coroutine_finish(lco, rc, nres, err);
}

/*
 * Resume the Lua coroutine with nil, error_message.
 */
static void
lua_http_on_error(int error_code, const char *error_msg, void *user)
{
	struct lua_http_req *lr = user;
	struct clm_lua_coroutine *lco;
	struct clm_lua_plugin *plugin;
	lua_State *co;
	int nres = 0, rc;

	if (lr->starting) {
		lr->start_result = LUA_HTTP_START_ERROR;
		(void)snprintf(lr->start_error, sizeof(lr->start_error), "%s",
		    error_msg ? error_msg : "unknown http error");
		return;
	}
	lco = lr->pending.coroutine;
	plugin = clm_lua_pending_remove(&lr->pending);
	if (plugin == NULL || lco == NULL) {
		lua_http_req_free(lr);
		return;
	}
	co = lco->co;
	if (lua_status(co) != LUA_YIELD) {
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		clm_lua_coroutine_finish(lco, LUA_ERRRUN, 0,
		    "lua http coroutine was not suspended");
		return;
	}
	lua_pushnil(co);
	lua_pushstring(co, error_msg ? error_msg : "unknown http error");
	uint64_t timeout = clm_tool_invocation_timeout_ms(lco->call->inv);
	rc = clm_lua_resume_with_deadline(plugin, co, lco->main_L, 2, &nres,
	    timeout);
	const char *err = rc == LUA_OK || rc == LUA_YIELD ? NULL :
	    lua_tostring(co, -1);
	if (rc != LUA_OK && rc != LUA_YIELD)
		clm_debug("lua http: coroutine error on error resume: %s",
		    err ? err : "(unknown)");
	(void)error_code;
	clm_lua_http_done(plugin);
	lua_http_req_free(lr);
	if (rc != LUA_YIELD)
		clm_lua_coroutine_finish(lco, rc, nres, err);
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

	if (!clm_lua_coroutine_is_valid(L))
		return luaL_error(L, "http.get may only be called from within "
		    "a tool coroutine");
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(L);
	plugin = lco->plugin;
	struct clm_agent *agent = lco->agent;
	if (agent == NULL || agent->host == NULL || agent->host->http_post == NULL)
		return luaL_error(L, "http_get: no http transport");

	lr = calloc(1, sizeof(*lr));
	if (lr == NULL)
		return luaL_error(L, "http_get: out of memory");
	lr->lco = lco;
	lr->agent = agent;

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
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_get: out of memory");
	}

	struct clm_http_req hreq = {
		.url = url,
		.api_key = NULL,
		.body = NULL,
		.headers = (const char *const *)hdrs,
		.client_suffix = lco->call->tool_name,
	};
	r = clm_lua_pending_add(plugin, L, &lr->pending, lua_http_teardown);
	if (r < 0) {
		free_headers(hdrs);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_get: invocation is shutting down");
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

	if (!clm_lua_coroutine_is_valid(L))
		return luaL_error(L, "http.post may only be called from within "
		    "a tool coroutine");
	struct clm_lua_coroutine *lco = clm_lua_coroutine_find(L);
	plugin = lco->plugin;
	struct clm_agent *agent = lco->agent;
	if (agent == NULL || agent->host == NULL || agent->host->http_post == NULL)
		return luaL_error(L, "http_post: no http transport");

	lr = calloc(1, sizeof(*lr));
	if (lr == NULL)
		return luaL_error(L, "http_post: out of memory");
	lr->lco = lco;
	lr->agent = agent;

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
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_post: out of memory");
	}

	struct clm_http_req hreq = {
		.url = url,
		.api_key = NULL,
		.body = body,
		.headers = (const char *const *)hdrs,
		.client_suffix = lco->call->tool_name,
	};
	r = clm_lua_pending_add(plugin, L, &lr->pending, lua_http_teardown);
	if (r < 0) {
		free_headers(hdrs);
		clm_lua_http_done(plugin);
		lua_http_req_free(lr);
		return luaL_error(L, "http_post: invocation is shutting down");
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
