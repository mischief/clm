// SPDX-License-Identifier: ISC
/*
 * Lua HTTP bindings: http.get(url) and http.post(url, body)
 * These yield the calling coroutine and resume it with the response
 * when the async HTTP request completes.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>

#include <uv.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/http_async.h"
#include "clm/http.h"
#include "clm/log.h"
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

/* State for one in-flight Lua HTTP request. */
struct lua_http_req {
	lua_State *co;              /* the yielded coroutine */
	lua_State *main_L;          /* plugin's main state */
	int co_ref;                 /* registry ref for the coroutine */
	struct clm_agent *agent;
	struct clm_http_request *req; /* the underlying async request */
	struct clm_lua_plugin *plugin; /* for budget tracking */
	const char *tool_name;      /* borrowed from invocation, for reporting */
	struct clm_tool_invocation *inv; /* for failing the tool on error */
};

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
	lua_State *co = lr->co;
	lua_State *L = lr->main_L;
	int nres, rc;

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
	rc = lua_resume(co, L, 1, &nres);
	if (rc == LUA_OK) {
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_budget_report(lr->plugin, lr->tool_name ? lr->tool_name : "?");
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
		clm_lua_budget_report(lr->plugin, lr->tool_name ? lr->tool_name : "?");
	}

	clm_lua_http_done(lr->plugin);
	free(lr);
}

/*
 * Resume the Lua coroutine with nil, error_message.
 */
static void
lua_http_on_error(int error_code, const char *error_msg, void *user)
{
	struct lua_http_req *lr = user;
	lua_State *co = lr->co;
	lua_State *L = lr->main_L;
	int nres, rc;

	/* Push nil + error string. */
	lua_pushnil(co);
	lua_pushstring(co, error_msg ? error_msg : "unknown HTTP error");

	nres = 0;
	rc = lua_resume(co, L, 2, &nres);
	if (rc == LUA_OK) {
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, lr->co_ref);
		clm_lua_budget_report(lr->plugin, lr->tool_name ? lr->tool_name : "?");
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
		clm_lua_budget_report(lr->plugin, lr->tool_name ? lr->tool_name : "?");
	}

	(void)error_code;
	clm_lua_http_done(lr->plugin);
	free(lr);
}

/* ------------------------------------------------------------------ */
/* Lua-callable HTTP functions (yield the coroutine)                    */
/* ------------------------------------------------------------------ */

/*
 * ctx:http_get(url [, headers_table])
 * Yields. On resume returns: response_table or nil, err.
 */
static int
lua_ctx_http_get(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	struct lua_http_req *lr;
	uv_loop_t *loop;
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

	loop = clm_tool_invocation_loop(NULL); /* We need the loop from agent */
	/* Actually get the loop from the agent's uv field directly. */
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_uv_loop");
	loop = lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (loop == NULL)
		return luaL_error(L, "http_get: no event loop");

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
	lr->plugin = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_tool_name");
	lr->tool_name = lua_tostring(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_inv");
	lr->inv = lua_touserdata(L, -1);
	lua_pop(L, 1);

	/* Enforce HTTP budget. */
	{
		int br = clm_lua_http_start(lr->plugin);
		if (br == -1) {
			free(lr);
			return luaL_error(L, "http_get: too many in-flight requests");
		}
		if (br == -2) {
			free(lr);
			return luaL_error(L, "http_get: request limit exceeded");
		}
	}

	/* Build optional request headers from arg 2 (table). */
	struct curl_slist *hdrs = NULL;
	if (lua_istable(L, 2)) {
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING &&
			    lua_type(L, -1) == LUA_TSTRING) {
				const char *k = lua_tostring(L, -2);
				const char *v = lua_tostring(L, -1);
				char hdr[512];
				(void)snprintf(hdr, sizeof(hdr), "%s: %s", k, v);
				hdrs = curl_slist_append(hdrs, hdr);
			}
			lua_pop(L, 1);
		}
	}

	r = clm_http_async_post(loop, url, NULL, NULL, hdrs,
	    lua_http_on_success, lua_http_on_error, NULL, lr->tool_name, lr,
	    &lr->req);
	if (r < 0) {
		free(lr);
		return luaL_error(L, "http_get: failed to start request: %s",
		    strerror(-r));
	}

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
	uv_loop_t *loop;
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

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_uv_loop");
	loop = lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (loop == NULL)
		return luaL_error(L, "http_post: no event loop");

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
	lr->plugin = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_tool_name");
	lr->tool_name = lua_tostring(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_inv");
	lr->inv = lua_touserdata(L, -1);
	lua_pop(L, 1);

	/* Enforce HTTP budget. */
	{
		int br = clm_lua_http_start(lr->plugin);
		if (br == -1) {
			free(lr);
			return luaL_error(L, "http_post: too many in-flight requests");
		}
		if (br == -2) {
			free(lr);
			return luaL_error(L, "http_post: request limit exceeded");
		}
	}

	/* Build request headers. Always set Content-Type for POST.
	 * If arg 3 is a table, add those as extra headers. */
	struct curl_slist *hdrs = NULL;
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	if (lua_istable(L, 3)) {
		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING &&
			    lua_type(L, -1) == LUA_TSTRING) {
				const char *k = lua_tostring(L, -2);
				const char *v = lua_tostring(L, -1);
				char hdr[512];
				(void)snprintf(hdr, sizeof(hdr), "%s: %s", k, v);
				hdrs = curl_slist_append(hdrs, hdr);
			}
			lua_pop(L, 1);
		}
	}

	r = clm_http_async_post(loop, url, NULL, body, hdrs,
	    lua_http_on_success, lua_http_on_error, NULL, lr->tool_name, lr,
	    &lr->req);
	if (r < 0) {
		free(lr);
		return luaL_error(L, "http_post: failed to start request: %s",
		    strerror(-r));
	}

	return lua_yield(L, 0);
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */

int
clm_lua_http_open(lua_State *L, struct clm_agent *agent)
{
	/* Store agent pointer and its uv loop in the registry for retrieval
	 * from within coroutines. */
	lua_pushlightuserdata(L, agent);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_agent");

	lua_pushlightuserdata(L, agent->uv);
	lua_setfield(L, LUA_REGISTRYINDEX, "_clm_uv_loop");

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
