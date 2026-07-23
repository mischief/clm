// SPDX-License-Identifier: ISC
/*
 * Lua subagent bindings: agent.new(opts) -> handle; handle:submit(prompt),
 * handle:free().
 *
 * A subagent is a second, independent struct clm_agent sharing the parent's
 * clm_host (transport + timers) -- host_uv's mux/loop are already agent-
 * agnostic (see host_uv.c), so no new loop or connection cache is needed.
 * clm_agent_new() itself registers the portable core's builtins
 * (read_file/write_file/list_dir) on the child, same as on any agent;
 * shell_exec/bg_exec live in libclmuv and are deliberately NOT wired here,
 * keeping this archive's link graph the same (lua_dep + clm_core_deps only,
 * see meson.build) -- a subagent that needs a shell is a later, explicit
 * decision, not a default.
 *
 * Same yield/resume pattern as lua_http.c and clm.sleep: handle:submit()
 * yields the calling tool-invocation coroutine; the child agent's own
 * on_turn_done callback resumes it once the child's turn lands. Only one
 * submit may be in flight per handle at a time.
 *
 * Permission handling is a prototype default only: on_permission auto-
 * allows once (mirrors the CLI's pre-gate behaviour, see main.c's
 * cb_permission), so a subagent's write_file etc. actually runs instead of
 * being denied by the library's secure default. A real embedder should
 * replace this with its own policy before this leaves prototype status.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/log.h"
#include "banned.h"

/* Invocation-thread guard + registry helpers, shared with lua_http.c
 * (defined in lua_plugin.c). */
int clm_lua_is_invocation_thread(lua_State *L);
void clm_lua_mark_invocation_thread(lua_State *L, lua_State *co, int on);
void clm_lua_clear_invocation_registry(lua_State *L);

/* Prototype -- called from lua_plugin.c. */
int clm_lua_agent_open(lua_State *L);

#define CLM_LUA_SUBAGENT_META "clm_subagent"
#define CLM_SUBAGENT_DEFAULT_MAX_ITER 10
#define CLM_SUBAGENT_MAX_MAX_ITER 50

struct lua_subagent {
	struct clm_agent *child; /* NULL once freed */
	lua_State *co;           /* coroutine parked in submit(), or NULL */
	lua_State *main_L;
	int co_ref;
	bool busy;
	char *last_text; /* most recent assistant content, overwritten */
	char *last_error;
};

static struct lua_subagent *
check_subagent(lua_State *L, int idx)
{
	return luaL_checkudata(L, idx, CLM_LUA_SUBAGENT_META);
}

/* ------------------------------------------------------------------ */
/* Child agent callbacks                                               */
/* ------------------------------------------------------------------ */

static void
sa_on_assistant_text(const char *text, void *user)
{
	struct lua_subagent *sa = user;
	char *copy = text ? strdup(text) : NULL;
	if (text != NULL && copy == NULL)
		return; /* OOM: keep whatever we had */
	free(sa->last_text);
	sa->last_text = copy;
}

static void
sa_on_permission(const struct clm_permission_req *req, void *user)
{
	struct lua_subagent *sa = user;
	/* Prototype default -- see file comment. */
	clm_tool_permission_respond(sa->child, req, CLM_PERM_ALLOW_ONCE);
}

static void
sa_on_turn_done(int status, void *user)
{
	struct lua_subagent *sa = user;
	lua_State *co = sa->co;
	lua_State *L = sa->main_L;
	int nres, rc;

	sa->busy = false;
	sa->co = NULL;

	/* Coroutine already gone (shouldn't happen, but mirror lua_http's
	 * defensive guard rather than crash unroll()ing a dead state). */
	if (co == NULL || lua_status(co) != LUA_YIELD) {
		clm_debug("subagent: coroutine not yielded, dropping turn result");
		return;
	}

	if (status != 0) {
		const char *err = clm_agent_get_last_error(sa->child);
		lua_pushnil(co);
		lua_pushstring(co, err ? err : "subagent turn failed");
		nres = 2;
	} else {
		if (sa->last_text != NULL)
			lua_pushstring(co, sa->last_text);
		else
			lua_pushstring(co, "");
		nres = 1;
	}

	rc = lua_resume(co, L, nres, &nres);
	if (rc == LUA_OK || rc == LUA_ERRRUN || rc == LUA_ERRMEM ||
	    rc == LUA_ERRERR) {
		clm_lua_mark_invocation_thread(L, co, 0);
		luaL_unref(L, LUA_REGISTRYINDEX, sa->co_ref);
		clm_lua_clear_invocation_registry(L);
		if (rc != LUA_OK) {
			const char *err = lua_tostring(co, -1);
			clm_debug("subagent: coroutine error on resume: %s",
			    err ? err : "(unknown)");
		}
	}
	/* rc == LUA_YIELD: coroutine yielded again (another submit/http/sleep);
	 * whatever it yielded into now owns co_ref until its own callback
	 * fires. */
}

static const struct clm_callbacks subagent_callbacks = {
	.on_assistant_text = sa_on_assistant_text,
	.on_permission = sa_on_permission,
	.on_turn_done = sa_on_turn_done,
};

/* ------------------------------------------------------------------ */
/* agent.new(opts)                                                     */
/* ------------------------------------------------------------------ */

static int
lua_agent_new(lua_State *L)
{
	struct clm_agent *parent;
	struct lua_subagent *sa;
	struct clm_cfg cfg = {0};
	lua_Integer max_iter = CLM_SUBAGENT_DEFAULT_MAX_ITER;
	int r;

	luaL_checktype(L, 1, LUA_TTABLE);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_agent");
	parent = lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (parent == NULL)
		return luaL_error(L, "agent.new: no parent agent context");

	lua_getfield(L, 1, "model");
	cfg.model = lua_isstring(L, -1) ? lua_tostring(L, -1)
	                                : clm_agent_get_model(parent);
	if (cfg.model == NULL)
		cfg.model = "local-model";

	lua_getfield(L, 1, "base_url");
	cfg.base_url = lua_isstring(L, -1) ? lua_tostring(L, -1)
	                                    : clm_agent_get_base_url(parent);

	lua_getfield(L, 1, "api_key");
	cfg.api_key = lua_isstring(L, -1) ? lua_tostring(L, -1)
	                                   : clm_agent_get_api_key(parent);
	if (cfg.api_key == NULL)
		cfg.api_key = "";

	lua_getfield(L, 1, "provider");
	if (lua_isstring(L, -1))
		cfg.provider = clm_provider_from_str(lua_tostring(L, -1));
	else
		cfg.provider = clm_agent_get_provider(parent);

	lua_getfield(L, 1, "system_prompt");
	cfg.system_prompt = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

	lua_getfield(L, 1, "max_iterations");
	if (lua_isinteger(L, -1))
		max_iter = lua_tointeger(L, -1);
	if (max_iter < 1)
		max_iter = 1;
	if (max_iter > CLM_SUBAGENT_MAX_MAX_ITER)
		max_iter = CLM_SUBAGENT_MAX_MAX_ITER;
	cfg.max_iterations = (size_t)max_iter;
	cfg.stream = false; /* on_assistant_text fires once with full content */

	/* Popped in reverse: 6 getfield pushes above (model, base_url,
	 * api_key, provider, system_prompt, max_iterations) = 6 values left
	 * on the stack. */
	lua_pop(L, 6);

	if (cfg.base_url == NULL)
		return luaL_error(L, "agent.new: no base_url (parent has none either)");

	sa = lua_newuserdatauv(L, sizeof(*sa), 0);
	memset(sa, 0, sizeof(*sa));
	luaL_setmetatable(L, CLM_LUA_SUBAGENT_META);

	/* clm_agent_new() already registers the portable builtins
	 * (read_file/write_file/list_dir) itself -- nothing more to add here. */
	r = clm_agent_new(&cfg, parent->host, &subagent_callbacks, sa, &sa->child);
	if (r < 0)
		return luaL_error(L, "agent.new: failed to create agent (%d)", r);

	return 1;
}

/* ------------------------------------------------------------------ */
/* handle:submit(prompt)                                               */
/* ------------------------------------------------------------------ */

static int
lua_agent_submit(lua_State *L)
{
	struct lua_subagent *sa = check_subagent(L, 1);
	const char *prompt = luaL_checkstring(L, 2);
	int r;

	if (sa->child == NULL)
		return luaL_error(L, "agent:submit: agent already freed");
	if (sa->busy)
		return luaL_error(L, "agent:submit: a submit is already in flight");
	if (!clm_lua_is_invocation_thread(L)) {
		return luaL_error(L, "agent:submit may only be called directly "
		    "from a tool invocation, not from a nested coroutine or at "
		    "load time");
	}

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_main_L");
	sa->main_L = lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_co_ref");
	sa->co_ref = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	free(sa->last_text);
	sa->last_text = NULL;

	r = clm_agent_submit(sa->child, prompt);
	if (r < 0)
		return luaL_error(L, "agent:submit: failed to start turn (%d)", r);

	sa->busy = true;
	sa->co = L;

	return lua_yield(L, 0);
}

/* handle:free() -- explicit teardown; also called from __gc as a backstop. */
static int
lua_agent_free(lua_State *L)
{
	struct lua_subagent *sa = check_subagent(L, 1);

	if (sa->busy)
		return luaL_error(L, "agent:free: a submit is still in flight");
	if (sa->child != NULL) {
		clm_agent_free(sa->child);
		sa->child = NULL;
	}
	free(sa->last_text);
	sa->last_text = NULL;
	return 0;
}

static int
lua_agent_gc(lua_State *L)
{
	struct lua_subagent *sa = check_subagent(L, 1);

	/* A subagent GC'd while still busy means the invocation coroutine
	 * that owns it is gone too (co_ref keeps it rooted otherwise) -- in
	 * that pathological case just leak the child rather than free an
	 * agent an in-flight callback might still touch. Normal teardown
	 * goes through handle:free(). */
	if (sa->busy)
		return 0;
	if (sa->child != NULL) {
		clm_agent_free(sa->child);
		sa->child = NULL;
	}
	free(sa->last_text);
	sa->last_text = NULL;
	return 0;
}

static const luaL_Reg subagent_methods[] = {
	{"submit", lua_agent_submit},
	{"free", lua_agent_free},
	{"__gc", lua_agent_gc},
	{NULL, NULL},
};

static const luaL_Reg agent_module[] = {
	{"new", lua_agent_new},
	{NULL, NULL},
};

int
clm_lua_agent_open(lua_State *L)
{
	luaL_newmetatable(L, CLM_LUA_SUBAGENT_META);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, subagent_methods, 0);
	lua_pop(L, 1);

	luaL_newlib(L, agent_module);
	lua_setglobal(L, "agent");

	return 0;
}
