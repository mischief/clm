// SPDX-License-Identifier: ISC
/*
 * clm.on(event, fn): plugin hooks on agent events. A pre_tool hook runs in
 * its own coroutine, so it may use http.get/post and clm.exec, and returns
 * nil, {allow, deny or ask = why} or {args = t}. turn_start and turn_end
 * hooks run as event callbacks and may not yield. See clm-tool(5).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include <cjson/cJSON.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/host.h"
#include "clm/log.h"
#include "lua_internal.h"
#include "banned.h"

void clm_lua_push_json_value(lua_State *L, cJSON *obj);

enum lua_hook_kind {
	LUA_HOOK_PRE_TOOL,
	LUA_HOOK_TURN_START,
	LUA_HOOK_TURN_END,
	LUA_HOOK_PROMPT,
};

struct lua_hook {
	TAILQ_ENTRY(lua_hook) entry;
	struct clm_lua_plugin *plugin;
	enum lua_hook_kind kind;
	int fn_ref;
	char *key; /* LUA_HOOK_PROMPT: the part this plugin set */
};

/* One pre_tool hook run: from the call into the hook to its answer. */
struct lua_gate_run {
	struct clm_lua_pending pending;
	struct clm_tool_gate *gate;
	struct clm_agent *agent;
	struct clm_timer *timer;
	bool decided;
	enum clm_gate_verdict verdict;
	char *reason;
	char *args;
};

static const char runner_src[] = "local finish = ...\n"
                                 "return function(fn, call, run)\n"
                                 "  local ok, r = pcall(fn, call)\n"
                                 "  finish(run, ok, r)\n"
                                 "end\n";

#define RUNNER_KEY "_clm_hook_runner"

static void
gate_run_free(struct lua_gate_run *run)
{
	free(run->reason);
	free(run->args);
	free(run);
}

/* Give the hook's answer to the agent, and free the run. */
static void
gate_run_answer(struct lua_gate_run *run)
{
	struct clm_tool_gate *gate = run->gate;
	enum clm_gate_verdict v = run->decided ? run->verdict : CLM_GATE_PASS;
	char *reason = run->reason, *args = run->args;

	run->reason = run->args = NULL;
	gate_run_free(run);
	(void)clm_tool_gate_respond(gate, v, reason, args);
	free(reason);
	free(args);
}

static void
gate_timer_cb(void *arg)
{
	struct lua_gate_run *run = arg;

	if (run->timer != NULL && run->agent->host->timer_cancel != NULL)
		run->agent->host->timer_cancel(run->timer);
	run->timer = NULL;
	(void)clm_lua_pending_remove(&run->pending);
	gate_run_answer(run);
}

/* The plugin is going away before the hook answered. */
static void
gate_run_teardown(struct clm_lua_pending *pending)
{
	struct lua_gate_run *run = (struct lua_gate_run *)pending;

	if (run->timer != NULL && run->agent->host->timer_cancel != NULL)
		run->agent->host->timer_cancel(run->timer);
	run->timer = NULL;
	if (!run->decided) {
		run->decided = true;
		run->verdict = CLM_GATE_DENY;
		run->reason = strdup("hook plugin unloaded");
	}
	gate_run_answer(run);
}

/* A table or string for new arguments, as JSON text. */
static char *
args_to_json(lua_State *L, int idx)
{
	const char *s;
	char *out;

	if (lua_type(L, idx) == LUA_TSTRING)
		return strdup(lua_tostring(L, idx));
	if (!lua_istable(L, idx))
		return NULL;
	idx = lua_absindex(L, idx);
	lua_getglobal(L, "json");
	lua_getfield(L, -1, "encode");
	lua_remove(L, -2);
	lua_pushvalue(L, idx);
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		lua_pop(L, 1);
		return NULL;
	}
	s = lua_tostring(L, -1);
	out = s != NULL ? strdup(s) : NULL;
	lua_pop(L, 1);
	return out;
}

/*
 * finish(run, ok, r), called by the runner when the hook returns. The answer
 * goes out from a timer, not from inside this coroutine: answering can start
 * the tool, and the tool may be a Lua tool of this same plugin.
 */
static int
lua_hook_finish(lua_State *L)
{
	struct lua_gate_run *run = lua_touserdata(L, 1);
	struct clm_host *host;

	if (run == NULL || run->decided)
		return 0;
	run->decided = true;
	run->verdict = CLM_GATE_PASS;
	if (!lua_toboolean(L, 2)) {
		const char *err = lua_tostring(L, 3);

		clm_debug("lua pre_tool hook: %s", err != NULL ? err : "error");
	} else if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "deny");
		lua_getfield(L, 3, "ask");
		if (lua_toboolean(L, -2)) {
			run->verdict = CLM_GATE_DENY;
			if (lua_type(L, -2) == LUA_TSTRING)
				run->reason = strdup(lua_tostring(L, -2));
		} else if (lua_toboolean(L, -1)) {
			run->verdict = CLM_GATE_ASK;
			if (lua_type(L, -1) == LUA_TSTRING)
				run->reason = strdup(lua_tostring(L, -1));
		}
		lua_pop(L, 2);
		lua_getfield(L, 3, "allow");
		if (run->verdict == CLM_GATE_PASS &&
		    lua_type(L, -1) == LUA_TSTRING)
			run->reason = strdup(lua_tostring(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, 3, "args");
		run->args = args_to_json(L, -1);
		lua_pop(L, 1);
	}

	host = run->agent->host;
	if (host->timer_set != NULL &&
	    host->timer_set(host->ctx, 0, gate_timer_cb, run, &run->timer) == 0)
		return 0;
	run->timer = NULL;
	(void)clm_lua_pending_remove(&run->pending);
	gate_run_answer(run);
	return 0;
}

struct hook_start {
	struct lua_hook *hook;
	struct lua_gate_run *run;
	const char *name;
	const char *args;
	lua_State *co;
	int co_ref;
};

/* Build the coroutine under lua_pcall, so running out of memory here
 * raises into a protected call. */
static int
hook_prepare(lua_State *L)
{
	struct hook_start *hs = lua_touserdata(L, 1);
	cJSON *args;

	hs->co = lua_newthread(L);
	hs->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_getfield(hs->co, LUA_REGISTRYINDEX, RUNNER_KEY);
	lua_rawgeti(hs->co, LUA_REGISTRYINDEX, hs->hook->fn_ref);
	lua_createtable(hs->co, 0, 2);
	lua_pushstring(hs->co, hs->name != NULL ? hs->name : "");
	lua_setfield(hs->co, -2, "name");
	args = cJSON_Parse(hs->args != NULL ? hs->args : "{}");
	if (args != NULL) {
		clm_lua_push_json_value(hs->co, args);
		cJSON_Delete(args);
	} else {
		lua_newtable(hs->co);
	}
	lua_setfield(hs->co, -2, "args");
	lua_pushlightuserdata(hs->co, hs->run);

	/* The async bindings read these, as in a tool call. */
	lua_pushinteger(hs->co, hs->co_ref);
	lua_setfield(hs->co, LUA_REGISTRYINDEX, "_clm_co_ref");
	lua_pushstring(hs->co, "pre_tool hook");
	lua_setfield(hs->co, LUA_REGISTRYINDEX, "_clm_tool_name");
	lua_pushnil(hs->co);
	lua_setfield(hs->co, LUA_REGISTRYINDEX, "_clm_inv");
	clm_lua_mark_invocation_thread(L, hs->co, 1);
	return 0;
}

static void
lua_pre_tool_hook(struct clm_tool_gate *gate, void *user)
{
	struct lua_hook *hook = user;
	struct clm_lua_plugin *plugin = hook->plugin;
	lua_State *L = clm_lua_plugin_state(plugin);
	struct hook_start hs = {0};
	struct lua_gate_run *run;
	int nres = 0, rc;

	if (!clm_lua_plugin_alive(plugin)) {
		(void)clm_tool_gate_respond(gate, CLM_GATE_PASS, NULL, NULL);
		return;
	}
	run = calloc(1, sizeof(*run));
	if (run == NULL) {
		(void)clm_tool_gate_respond(gate, CLM_GATE_PASS, NULL, NULL);
		return;
	}
	run->gate = gate;
	run->agent = clm_lua_plugin_agent(plugin);
	if (clm_lua_pending_add(plugin, &run->pending, gate_run_teardown) < 0) {
		gate_run_answer(run);
		return;
	}

	hs.hook = hook;
	hs.run = run;
	hs.name = clm_tool_gate_name(gate);
	hs.args = clm_tool_gate_args(gate);
	lua_pushcfunction(L, hook_prepare);
	lua_pushlightuserdata(L, &hs);
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		clm_debug("lua pre_tool hook: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		if (hs.co != NULL) {
			clm_lua_mark_invocation_thread(L, hs.co, 0);
			luaL_unref(L, LUA_REGISTRYINDEX, hs.co_ref);
		}
		(void)clm_lua_pending_remove(&run->pending);
		gate_run_answer(run);
		return;
	}

	rc = clm_lua_resume_with_deadline(plugin, hs.co, L, 3, &nres, 0);
	if (rc == LUA_YIELD)
		return; /* an async binding resumes it */
	if (rc != LUA_OK)
		clm_debug("lua pre_tool hook: %s", lua_tostring(hs.co, -1));
	clm_lua_mark_invocation_thread(L, hs.co, 0);
	luaL_unref(L, LUA_REGISTRYINDEX, hs.co_ref);
	clm_lua_clear_invocation_registry(L);
	/* The runner calls finish, which answers; an error before it did
	 * leaves the run pending, so answer it here. */
	if (rc != LUA_OK && !run->decided) {
		(void)clm_lua_pending_remove(&run->pending);
		gate_run_answer(run);
	}
}

struct turn_push {
	struct lua_hook *hook;
	const struct clm_turn_info *info;
};

static int
push_turn(lua_State *L, void *arg)
{
	struct turn_push *tp = arg;

	lua_rawgeti(L, LUA_REGISTRYINDEX, tp->hook->fn_ref);
	lua_createtable(L, 0, 2);
	if (tp->info->event == CLM_TURN_START) {
		lua_pushstring(L, tp->info->prompt);
		lua_setfield(L, -2, "prompt");
	} else {
		lua_pushinteger(L, tp->info->status);
		lua_setfield(L, -2, "status");
		if (tp->info->text != NULL) {
			lua_pushstring(L, tp->info->text);
			lua_setfield(L, -2, "text");
		}
	}
	return 1;
}

static void
lua_turn_hook(const struct clm_turn_info *info, void *user)
{
	struct lua_hook *hook = user;
	struct turn_push tp = {hook, info};

	if ((hook->kind == LUA_HOOK_TURN_START) !=
	    (info->event == CLM_TURN_START))
		return;
	(void)clm_lua_plugin_callback(hook->plugin, push_turn, &tp);
}

/* clm.on(event, fn) */
static int
lua_clm_on(lua_State *L)
{
	struct clm_lua_plugin *plugin = lua_touserdata(L, lua_upvalueindex(1));
	static const char *const events[] = {
	    "pre_tool", "turn_start", "turn_end", NULL};
	enum lua_hook_kind kind =
	    (enum lua_hook_kind)luaL_checkoption(L, 1, NULL, events);
	struct clm_agent *agent = clm_lua_plugin_agent(plugin);
	struct lua_hook *hook;
	int r;

	luaL_checktype(L, 2, LUA_TFUNCTION);
	hook = calloc(1, sizeof(*hook));
	if (hook == NULL)
		return luaL_error(L, "clm.on: out of memory");
	hook->plugin = plugin;
	hook->kind = kind;
	lua_pushvalue(L, 2);
	hook->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if (kind == LUA_HOOK_PRE_TOOL)
		r = clm_agent_add_pre_tool_hook(agent, lua_pre_tool_hook, hook);
	else
		r = clm_agent_add_turn_hook(agent, lua_turn_hook, hook);
	if (r < 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, hook->fn_ref);
		free(hook);
		return luaL_error(L, "clm.on: %s", strerror(-r));
	}
	TAILQ_INSERT_TAIL(clm_lua_plugin_hooks(plugin), hook, entry);
	return 0;
}

/* clm.prompt_set(key, text): a system prompt part, removed with nil. */
static int
lua_clm_prompt_set(lua_State *L)
{
	struct clm_lua_plugin *plugin = lua_touserdata(L, lua_upvalueindex(1));
	struct clm_lua_hook_list *hooks = clm_lua_plugin_hooks(plugin);
	const char *key = luaL_checkstring(L, 1);
	const char *text = luaL_optstring(L, 2, NULL);
	struct lua_hook *hook;
	int r;

	TAILQ_FOREACH(hook, hooks, entry)
	if (hook->kind == LUA_HOOK_PROMPT && strcmp(hook->key, key) == 0)
		break;
	if (hook == NULL && text != NULL) {
		hook = calloc(1, sizeof(*hook));
		if (hook == NULL || (hook->key = strdup(key)) == NULL) {
			free(hook);
			return luaL_error(L, "clm.prompt_set: out of memory");
		}
		hook->plugin = plugin;
		hook->kind = LUA_HOOK_PROMPT;
		hook->fn_ref = LUA_NOREF;
		TAILQ_INSERT_TAIL(hooks, hook, entry);
	}
	r = clm_agent_set_prompt_part(clm_lua_plugin_agent(plugin), key, text);
	if (r < 0)
		return luaL_error(L, "clm.prompt_set: %s", strerror(-r));
	if (hook != NULL && text == NULL) {
		TAILQ_REMOVE(hooks, hook, entry);
		free(hook->key);
		free(hook);
	}
	return 0;
}

void
clm_lua_hook_open(lua_State *L, struct clm_lua_plugin *plugin)
{
	lua_pushlightuserdata(L, plugin);
	lua_pushcclosure(L, lua_clm_on, 1);
	lua_setfield(L, -2, "on");
	lua_pushlightuserdata(L, plugin);
	lua_pushcclosure(L, lua_clm_prompt_set, 1);
	lua_setfield(L, -2, "prompt_set");

	/* The runner, closed over finish. */
	if (luaL_loadstring(L, runner_src) == LUA_OK) {
		lua_pushcfunction(L, lua_hook_finish);
		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			lua_setfield(L, LUA_REGISTRYINDEX, RUNNER_KEY);
			return;
		}
	}
	clm_debug("lua: hook runner: %s", lua_tostring(L, -1));
	lua_pop(L, 1);
}

void
clm_lua_hook_drop_all(struct clm_lua_plugin *plugin)
{
	struct clm_lua_hook_list *hooks = clm_lua_plugin_hooks(plugin);
	struct clm_agent *agent = clm_lua_plugin_agent(plugin);
	struct lua_hook *hook;

	while ((hook = TAILQ_FIRST(hooks)) != NULL) {
		TAILQ_REMOVE(hooks, hook, entry);
		if (hook->kind == LUA_HOOK_PROMPT) {
			(void)clm_agent_set_prompt_part(agent, hook->key, NULL);
			free(hook->key);
			free(hook);
			continue;
		}
		if (hook->kind == LUA_HOOK_PRE_TOOL)
			(void)clm_agent_remove_pre_tool_hook(
			    agent, lua_pre_tool_hook, hook);
		else
			(void)clm_agent_remove_turn_hook(
			    agent, lua_turn_hook, hook);
		if (clm_lua_plugin_alive(plugin))
			luaL_unref(clm_lua_plugin_state(plugin),
			    LUA_REGISTRYINDEX, hook->fn_ref);
		free(hook);
	}
}
