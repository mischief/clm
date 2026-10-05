// SPDX-License-Identifier: ISC
/*
 * require("clm"): drive clm agents from a Lua script. Turns, sleeps, tools
 * and pre_tool hooks run in coroutines that the libuv loop resumes while
 * clm.run drives it. See lua/README.md.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <lauxlib.h>
#include <lua.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/history.h"
#include "clm/host_uv.h"

#define AGENT_META "clm.agent"
#define TASKS_KEY "clm.tasks"

cJSON *clm_lua_to_cjson(lua_State *L, int idx);
void clm_lua_push_json_value(lua_State *L, cJSON *obj);

static uv_loop_t loop;
static lua_State *mainL;

/* ------------------------------------------------------------------ */
/* Tasks: coroutines this module starts, with a hook for their end     */
/* ------------------------------------------------------------------ */

struct task {
	lua_State *co;
	int ref;
	void (*finish)(struct task *t, bool ok, int nres);
	void *user;
	bool keep; /* the starter reads the result, then frees it */
	bool done;
	bool ok;
	char *err;
};

static void
report(const char *what, const char *msg)
{
	fprintf(stderr, "clm: %s: %s\n", what, msg != NULL ? msg : "error");
}

static struct task *
task_of(lua_State *co)
{
	struct task *t;

	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(co);
	lua_xmove(co, mainL, 1);
	lua_rawget(mainL, -2);
	t = lua_touserdata(mainL, -1);
	lua_pop(mainL, 2);
	return t;
}

static void
task_forget(struct task *t)
{
	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(t->co);
	lua_xmove(t->co, mainL, 1);
	lua_pushnil(mainL);
	lua_rawset(mainL, -3);
	lua_pop(mainL, 1);
	luaL_unref(mainL, LUA_REGISTRYINDEX, t->ref);
}

/* Resume co with the nargs values on its stack. A task that ends runs its
 * finish hook; a coroutine this module did not start is just resumed. */
static void
co_resume(lua_State *co, int nargs)
{
	struct task *t = task_of(co);
	int nres = 0, rc = lua_resume(co, mainL, nargs, &nres);

	if (rc == LUA_YIELD)
		return;
	if (t == NULL) {
		if (rc != LUA_OK)
			report("coroutine", lua_tostring(co, -1));
		return;
	}
	t->done = true;
	t->ok = rc == LUA_OK;
	if (!t->ok) {
		const char *m = lua_tostring(co, -1);

		t->err = strdup(m != NULL ? m : "error");
		nres = 1;
	}
	if (t->finish != NULL)
		t->finish(t, t->ok, nres);
	else if (!t->ok && !t->keep)
		report("task", t->err);
	task_forget(t);
	if (t->keep)
		return;
	free(t->err);
	free(t);
}

/* Start fn (at idx on L) with nargs arguments above it in a new coroutine. */
static struct task *
task_start(lua_State *L, int nargs, void (*finish)(struct task *, bool, int),
    void *user, bool keep)
{
	struct task *t = calloc(1, sizeof(*t));

	if (t == NULL) {
		lua_pop(L, nargs + 1);
		return NULL;
	}
	t->keep = keep;
	t->co = lua_newthread(mainL);
	t->ref = luaL_ref(mainL, LUA_REGISTRYINDEX);
	t->finish = finish;
	t->user = user;
	lua_xmove(L, t->co, nargs + 1);
	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(t->co);
	lua_xmove(t->co, mainL, 1);
	lua_pushlightuserdata(mainL, t);
	lua_rawset(mainL, -3);
	lua_pop(mainL, 1);
	co_resume(t->co, nargs);
	return t;
}

/* A coroutine parked until a callback resumes it. */
struct wait {
	lua_State *co;
	int ref;
};

static int
wait_begin(lua_State *L, struct wait *w)
{
	if (L == mainL || !lua_isyieldable(L))
		return -1;
	w->co = L;
	lua_pushthread(L);
	lua_xmove(L, mainL, 1);
	w->ref = luaL_ref(mainL, LUA_REGISTRYINDEX);
	return 0;
}

static void
wait_end(struct wait *w, int nargs)
{
	lua_State *co = w->co;

	luaL_unref(mainL, LUA_REGISTRYINDEX, w->ref);
	w->co = NULL;
	co_resume(co, nargs);
}

/* ------------------------------------------------------------------ */
/* Agents                                                              */
/* ------------------------------------------------------------------ */

#define MAX_STRS 8

struct lagent {
	struct clm_agent *agent;
	struct clm_host *host;
	int opts;  /* registry ref: the options table, callbacks included */
	int hooks; /* registry ref: {pre_tool = {...}, turn_start = ...} */
	char *strs[MAX_STRS];
	size_t nstrs;
	bool busy;
	int status;
	char *text;
	struct wait waiter;
	bool sync_wait;
};

static struct lagent *
check_agent(lua_State *L, int idx)
{
	struct lagent *la = luaL_checkudata(L, idx, AGENT_META);

	if (la->agent == NULL)
		luaL_error(L, "clm: agent is closed");
	return la;
}

/* Push opts[name] on mainL; true if it is a function. */
static bool
push_cb(struct lagent *la, const char *name)
{
	lua_rawgeti(mainL, LUA_REGISTRYINDEX, la->opts);
	lua_getfield(mainL, -1, name);
	lua_remove(mainL, -2);
	if (lua_isfunction(mainL, -1))
		return true;
	lua_pop(mainL, 1);
	return false;
}

static void
call_cb(const char *name, int nargs)
{
	if (lua_pcall(mainL, nargs, 0, 0) != LUA_OK) {
		report(name, lua_tostring(mainL, -1));
		lua_pop(mainL, 1);
	}
}

static void
cb_text(const char *text, void *user)
{
	if (!push_cb(user, "on_text"))
		return;
	lua_pushstring(mainL, text);
	call_cb("on_text", 1);
}

static void
cb_reasoning(const char *text, void *user)
{
	if (!push_cb(user, "on_reasoning"))
		return;
	lua_pushstring(mainL, text);
	call_cb("on_reasoning", 1);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	if (!push_cb(user, "on_tool_begin"))
		return;
	lua_pushstring(mainL, name);
	lua_pushstring(mainL, args != NULL ? args : "{}");
	call_cb("on_tool_begin", 2);
}

static void
cb_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	static const char *const names[] = {"ok", "failed", "timedout"};

	if (!push_cb(user, "on_tool"))
		return;
	lua_pushstring(mainL, name);
	lua_pushstring(mainL, content != NULL ? content : "");
	lua_pushstring(
	    mainL, (unsigned)outcome < 3 ? names[outcome] : "failed");
	call_cb("on_tool", 3);
}

static void
cb_notice(const char *text, void *user)
{
	if (!push_cb(user, "on_notice"))
		return;
	lua_pushstring(mainL, text);
	call_cb("on_notice", 1);
}

static void
cb_usage(const struct clm_usage *u, void *user)
{
	if (!push_cb(user, "on_usage"))
		return;
	lua_createtable(mainL, 0, 4);
	lua_pushinteger(mainL, u->prompt_tokens);
	lua_setfield(mainL, -2, "prompt");
	lua_pushinteger(mainL, u->completion_tokens);
	lua_setfield(mainL, -2, "completion");
	lua_pushinteger(mainL, u->cache_read_tokens);
	lua_setfield(mainL, -2, "cached");
	lua_pushinteger(mainL, u->cache_write_tokens);
	lua_setfield(mainL, -2, "cache_written");
	call_cb("on_usage", 1);
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct lagent *la = user;
	const char *reason = clm_permission_req_reason(req);
	bool allow = false;

	if (push_cb(la, "permission")) {
		lua_pushstring(mainL, clm_permission_req_name(req));
		lua_pushstring(mainL, clm_permission_req_args(req));
		if (reason != NULL)
			lua_pushstring(mainL, reason);
		else
			lua_pushnil(mainL);
		if (lua_pcall(mainL, 3, 1, 0) != LUA_OK) {
			report("permission", lua_tostring(mainL, -1));
		} else {
			allow = lua_toboolean(mainL, -1);
		}
		lua_pop(mainL, 1);
	} else {
		/* "allow" answers yes, except when a hook asked for a
		 * person: no one is here to answer. */
		lua_rawgeti(mainL, LUA_REGISTRYINDEX, la->opts);
		lua_getfield(mainL, -1, "permission");
		allow = reason == NULL && lua_isstring(mainL, -1) &&
		    strcmp(lua_tostring(mainL, -1), "allow") == 0;
		lua_pop(mainL, 2);
	}
	clm_tool_permission_respond(
	    la->agent, req, allow ? CLM_PERM_ALLOW_ONCE : CLM_PERM_DENY_ONCE);
}

static void
push_result(struct lagent *la)
{
	if (la->status == 0) {
		lua_pushstring(mainL, la->text != NULL ? la->text : "");
		lua_pushinteger(mainL, 0);
	} else {
		const char *e = clm_agent_get_last_error(la->agent);

		lua_pushnil(mainL);
		lua_pushstring(mainL,
		    e != NULL && e[0] != '\0'
		        ? e
		        : (la->status == -ECANCELED ? "cancelled" : "failed"));
	}
	lua_pushinteger(mainL, la->status);
}

static void
cb_turn_done(int status, void *user)
{
	struct lagent *la = user;

	la->busy = false;
	la->status = status;
	if (la->waiter.co != NULL) {
		push_result(la);
		lua_xmove(mainL, la->waiter.co, 3);
		wait_end(&la->waiter, 3);
	}
}

static const struct clm_callbacks callbacks = {
    .on_assistant_text = cb_text,
    .on_reasoning = cb_reasoning,
    .on_tool_begin = cb_tool_begin,
    .on_permission = cb_permission,
    .on_tool_result = cb_tool_result,
    .on_usage = cb_usage,
    .on_turn_done = cb_turn_done,
    .on_notice = cb_notice,
};

static void
turn_hook(const struct clm_turn_info *info, void *user)
{
	struct lagent *la = user;
	const char *ev =
	    info->event == CLM_TURN_START ? "turn_start" : "turn_end";
	int n, i;

	if (info->event == CLM_TURN_END) {
		free(la->text);
		la->text = info->text != NULL ? strdup(info->text) : NULL;
	}
	lua_rawgeti(mainL, LUA_REGISTRYINDEX, la->hooks);
	lua_getfield(mainL, -1, ev);
	n = lua_istable(mainL, -1) ? (int)lua_rawlen(mainL, -1) : 0;
	for (i = 1; i <= n; i++) {
		lua_rawgeti(mainL, -1, i);
		lua_createtable(mainL, 0, 2);
		if (info->event == CLM_TURN_START) {
			lua_pushstring(mainL, info->prompt);
			lua_setfield(mainL, -2, "prompt");
		} else {
			lua_pushinteger(mainL, info->status);
			lua_setfield(mainL, -2, "status");
			if (info->text != NULL) {
				lua_pushstring(mainL, info->text);
				lua_setfield(mainL, -2, "text");
			}
		}
		call_cb(ev, 1);
	}
	lua_pop(mainL, 2);
}

/* pre_tool: each hook runs as a task; its return value answers the gate. */
struct gate_task {
	struct clm_tool_gate *gate;
};

static void
gate_finish(struct task *t, bool ok, int nres)
{
	struct gate_task *g = t->user;
	lua_State *co = t->co;
	enum clm_gate_verdict v = CLM_GATE_PASS;
	const char *reason = NULL;
	char *args = NULL;
	int top = lua_gettop(co);

	if (!ok) {
		report("pre_tool", t->err);
	} else if (nres > 0 && lua_istable(co, top - nres + 1)) {
		int r = top - nres + 1;

		lua_getfield(co, r, "deny");
		lua_getfield(co, r, "ask");
		lua_getfield(co, r, "allow");
		if (lua_toboolean(co, -3)) {
			v = CLM_GATE_DENY;
			reason = lua_tostring(co, -3);
		} else if (lua_toboolean(co, -2)) {
			v = CLM_GATE_ASK;
			reason = lua_tostring(co, -2);
		} else {
			reason = lua_tostring(co, -1);
		}
		lua_getfield(co, r, "args");
		if (lua_istable(co, -1)) {
			cJSON *j = clm_lua_to_cjson(co, -1);

			args = j != NULL ? cJSON_PrintUnformatted(j) : NULL;
			cJSON_Delete(j);
		} else if (lua_isstring(co, -1)) {
			args = strdup(lua_tostring(co, -1));
		}
	}
	(void)clm_tool_gate_respond(g->gate, v, reason, args);
	free(args);
	free(g);
}

static void
pre_tool_hook(struct clm_tool_gate *gate, void *user)
{
	struct lagent *la = user;
	struct gate_task *g = calloc(1, sizeof(*g));
	cJSON *args;

	if (g == NULL) {
		(void)clm_tool_gate_respond(gate, CLM_GATE_PASS, NULL, NULL);
		return;
	}
	g->gate = gate;
	lua_rawgeti(mainL, LUA_REGISTRYINDEX, la->hooks);
	lua_getfield(mainL, -1, "pre_tool");
	lua_rawgeti(mainL, -1, 1);
	lua_remove(mainL, -2);
	lua_remove(mainL, -2);
	lua_createtable(mainL, 0, 2);
	lua_pushstring(mainL, clm_tool_gate_name(gate));
	lua_setfield(mainL, -2, "name");
	args = cJSON_Parse(clm_tool_gate_args(gate));
	if (args != NULL) {
		clm_lua_push_json_value(mainL, args);
		cJSON_Delete(args);
	} else {
		lua_newtable(mainL);
	}
	lua_setfield(mainL, -2, "args");
	(void)task_start(mainL, 1, gate_finish, g, false);
}

/* Tools written in Lua: invoke(args) returns the result, or raises. */
struct ltool {
	int fn;
};

static void
tool_finish(struct task *t, bool ok, int nres)
{
	struct clm_tool_invocation *inv = t->user;
	lua_State *co = t->co;

	if (!ok) {
		clm_tool_fail(inv, t->err);
	} else if (nres == 0 || lua_isnil(co, -nres)) {
		clm_tool_complete(inv, "");
	} else {
		size_t len;
		const char *s = luaL_tolstring(co, -nres, &len);
		struct clm_buffer b = {(const uint8_t *)s, len};

		clm_tool_complete_buf(inv, b);
		lua_pop(co, 1);
	}
}

static void
tool_invoke(struct clm_tool_invocation *inv, void *user)
{
	struct ltool *lt = user;
	cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));

	lua_rawgeti(mainL, LUA_REGISTRYINDEX, lt->fn);
	if (args != NULL) {
		clm_lua_push_json_value(mainL, args);
		cJSON_Delete(args);
	} else {
		lua_newtable(mainL);
	}
	if (task_start(mainL, 1, tool_finish, inv, false) == NULL)
		clm_tool_fail(inv, "out of memory");
}

static void
tool_detach(void *user)
{
	struct ltool *lt = user;

	if (mainL != NULL)
		luaL_unref(mainL, LUA_REGISTRYINDEX, lt->fn);
	free(lt);
}

static const char *
keep_str(struct lagent *la, lua_State *L, int t, const char *key)
{
	const char *s;

	lua_getfield(L, t, key);
	s = lua_tostring(L, -1);
	lua_pop(L, 1);
	if (s == NULL || la->nstrs == MAX_STRS)
		return NULL;
	la->strs[la->nstrs] = strdup(s);
	return la->strs[la->nstrs++];
}

static lua_Integer
opt_int(lua_State *L, int t, const char *key, lua_Integer dflt)
{
	lua_Integer v;

	lua_getfield(L, t, key);
	v = lua_isnil(L, -1) ? dflt : luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	return v;
}

static void
agent_close(struct lagent *la)
{
	if (la->agent != NULL) {
		clm_agent_free(la->agent);
		la->agent = NULL;
	}
	/* Freeing mutes callbacks, so end a waiting turn here. */
	if (la->waiter.co != NULL && mainL != NULL) {
		la->busy = false;
		lua_pushnil(mainL);
		lua_pushstring(mainL, "agent closed");
		lua_pushinteger(mainL, -ECANCELED);
		lua_xmove(mainL, la->waiter.co, 3);
		wait_end(&la->waiter, 3);
	}
	if (la->host != NULL) {
		clm_host_uv_free(la->host);
		la->host = NULL;
	}
	for (size_t i = 0; i < la->nstrs; i++)
		free(la->strs[i]);
	la->nstrs = 0;
	free(la->text);
	la->text = NULL;
	if (mainL != NULL) {
		luaL_unref(mainL, LUA_REGISTRYINDEX, la->opts);
		luaL_unref(mainL, LUA_REGISTRYINDEX, la->hooks);
	}
	la->opts = la->hooks = LUA_NOREF;
}

/* clm.agent{url=, model=, provider=, api_key=, system_prompt=, stream=,
 * effort=, context_size=, max_iterations=, tools={"shell",...},
 * permission="allow"|"deny"|fn, on_text=fn, ...} */
static int
l_agent(lua_State *L)
{
	struct lagent *la;
	struct clm_cfg cfg = {0};
	const char *url, *kind, *effort;
	char endpoint[512];
	int r;

	luaL_checktype(L, 1, LUA_TTABLE);
	la = lua_newuserdatauv(L, sizeof(*la), 0);
	memset(la, 0, sizeof(*la));
	la->opts = la->hooks = LUA_NOREF;
	luaL_setmetatable(L, AGENT_META);

	url = keep_str(la, L, 1, "url");
	if (url == NULL)
		return luaL_error(L, "clm.agent: url is required");
	kind = keep_str(la, L, 1, "provider");
	cfg.provider = clm_provider_from_str(kind);
	clm_provider_build_url(endpoint, sizeof(endpoint), url, cfg.provider);
	cfg.base_url = la->nstrs < MAX_STRS
	    ? (la->strs[la->nstrs++] = strdup(endpoint))
	    : url;
	cfg.model = keep_str(la, L, 1, "model");
	cfg.api_key = keep_str(la, L, 1, "api_key");
	if (cfg.api_key == NULL)
		cfg.api_key = ""; /* a local server needs none */
	cfg.system_prompt = keep_str(la, L, 1, "system_prompt");
	effort = keep_str(la, L, 1, "effort");
	lua_getfield(L, 1, "stream");
	cfg.stream = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
	lua_pop(L, 1);
	cfg.context_size = opt_int(L, 1, "context_size", 0);
	cfg.max_iterations = (size_t)opt_int(L, 1, "max_iterations", 0);

	lua_pushvalue(L, 1);
	la->opts = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L);
	la->hooks = luaL_ref(L, LUA_REGISTRYINDEX);

	r = clm_host_uv_new(&loop, &la->host);
	if (r == 0)
		r = clm_agent_new(&cfg, la->host, &callbacks, la, &la->agent);
	if (r < 0) {
		agent_close(la);
		return luaL_error(L, "clm.agent: %s", strerror(-r));
	}
	if (effort != NULL)
		(void)clm_agent_set_effort(la->agent, effort);
	(void)clm_agent_add_turn_hook(la->agent, turn_hook, la);

	lua_getfield(L, 1, "tools");
	if (lua_istable(L, -1)) {
		for (int i = 1; i <= (int)lua_rawlen(L, -1); i++) {
			const char *t;

			lua_rawgeti(L, -1, i);
			t = lua_tostring(L, -1);
			if (t != NULL && strcmp(t, "shell") == 0)
				(void)clm_tools_register_shell(la->agent);
			else if (t != NULL && strcmp(t, "bg") == 0)
				(void)clm_tools_register_bg(la->agent);
			else if (t != NULL && strcmp(t, "monitor") == 0)
				(void)clm_tools_register_monitor(la->agent);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return 1;
}

/* agent:turn(prompt) -> text, 0 | nil, err, status. Inside a coroutine
 * it yields until the turn ends; outside one it runs the loop itself. */
static int
l_turn_k(lua_State *L, int status, lua_KContext ctx)
{
	(void)status;
	(void)ctx;
	return 3;
}

static int
l_turn(lua_State *L)
{
	struct lagent *la = check_agent(L, 1);
	const char *prompt = luaL_checkstring(L, 2);
	int r;

	if (la->busy) {
		lua_pushnil(L);
		lua_pushstring(L, "a turn is already running");
		lua_pushinteger(L, -EBUSY);
		return 3;
	}
	free(la->text);
	la->text = NULL;
	la->busy = true;
	if (wait_begin(L, &la->waiter) == 0) {
		r = clm_agent_submit(la->agent, prompt);
		if (r < 0) {
			luaL_unref(mainL, LUA_REGISTRYINDEX, la->waiter.ref);
			la->waiter.co = NULL;
			la->busy = false;
			la->status = r;
			push_result(la);
			lua_xmove(mainL, L, 3);
			return 3;
		}
		return lua_yieldk(L, 0, 0, l_turn_k);
	}
	r = clm_agent_submit(la->agent, prompt);
	if (r < 0) {
		la->busy = false;
		la->status = r;
	}
	while (la->busy)
		uv_run(&loop, UV_RUN_ONCE);
	push_result(la);
	lua_xmove(mainL, L, 3);
	return 3;
}

static int
l_notify(lua_State *L)
{
	struct lagent *la = check_agent(L, 1);
	int r = clm_agent_notify(la->agent, luaL_checkstring(L, 2));

	lua_pushboolean(L, r == 0);
	return 1;
}

static int
l_cancel(lua_State *L)
{
	lua_pushboolean(L, clm_agent_cancel(check_agent(L, 1)->agent) == 0);
	return 1;
}

/* agent:tool{name=, description=, params=, invoke=fn, no_prompt=,
 * timeout_ms=} */
static int
l_tool(lua_State *L)
{
	struct lagent *la = check_agent(L, 1);
	struct clm_tool_def def = {0};
	struct ltool *lt;
	char *schema = NULL;
	int r;

	luaL_checktype(L, 2, LUA_TTABLE);
	lua_getfield(L, 2, "name");
	def.name = luaL_checkstring(L, -1);
	lua_getfield(L, 2, "description");
	def.description = lua_tostring(L, -1);
	lua_getfield(L, 2, "params");
	if (lua_istable(L, -1)) {
		cJSON *j = clm_lua_to_cjson(L, -1);

		schema = j != NULL ? cJSON_PrintUnformatted(j) : NULL;
		cJSON_Delete(j);
	}
	def.params_schema =
	    schema != NULL ? schema : "{\"type\":\"object\",\"properties\":{}}";
	lua_getfield(L, 2, "no_prompt");
	def.flags = lua_toboolean(L, -1) ? CLM_TOOL_NO_PROMPT : 0;
	def.timeout_ms = (uint64_t)opt_int(L, 2, "timeout_ms", 0);
	lua_getfield(L, 2, "invoke");
	luaL_checktype(L, -1, LUA_TFUNCTION);
	lt = malloc(sizeof(*lt));
	if (lt == NULL) {
		free(schema);
		return luaL_error(L, "clm: out of memory");
	}
	lt->fn = luaL_ref(L, LUA_REGISTRYINDEX);
	def.invoke = tool_invoke;
	def.detach = tool_detach;
	def.user = lt;
	r = clm_tool_add(la->agent, &def);
	free(schema);
	if (r < 0) {
		tool_detach(lt);
		return luaL_error(
		    L, "clm: tool %s: %s", def.name, strerror(-r));
	}
	return 0;
}

/* agent:on("pre_tool"|"turn_start"|"turn_end", fn) */
static int
l_on(lua_State *L)
{
	static const char *const events[] = {
	    "pre_tool", "turn_start", "turn_end", NULL};
	struct lagent *la = check_agent(L, 1);
	int ev = luaL_checkoption(L, 2, NULL, events);
	bool first;

	luaL_checktype(L, 3, LUA_TFUNCTION);
	lua_rawgeti(L, LUA_REGISTRYINDEX, la->hooks);
	lua_getfield(L, -1, events[ev]);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, events[ev]);
	}
	first = lua_rawlen(L, -1) == 0;
	if (ev == 0 && !first)
		return luaL_error(L, "clm: one pre_tool hook per agent");
	lua_pushvalue(L, 3);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	if (ev == 0 && first)
		(void)clm_agent_add_pre_tool_hook(la->agent, pre_tool_hook, la);
	return 0;
}

static int
l_prompt_set(lua_State *L)
{
	struct lagent *la = check_agent(L, 1);
	int r = clm_agent_set_prompt_part(
	    la->agent, luaL_checkstring(L, 2), luaL_optstring(L, 3, NULL));

	if (r < 0)
		return luaL_error(L, "clm: prompt_set: %s", strerror(-r));
	return 0;
}

static int
l_history(lua_State *L)
{
	struct lagent *la = check_agent(L, 1);
	const struct clm_message *m;
	int i = 0;

	lua_newtable(L);
	TAILQ_FOREACH(m, clm_agent_get_history(la->agent), entries)
	{
		cJSON *j = clm_message_to_json_full(m, NULL);

		if (j == NULL)
			continue;
		clm_lua_push_json_value(L, j);
		cJSON_Delete(j);
		lua_rawseti(L, -2, ++i);
	}
	return 1;
}

/* agent:clear(): start a fresh conversation; not while a turn runs. */
static int
l_clear(lua_State *L)
{
	int r = clm_agent_clear_history(check_agent(L, 1)->agent);

	if (r < 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(-r));
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int
l_state(lua_State *L)
{
	static const char *const names[] = {"idle", "thinking", "calling_tool",
	    "rate_limited", "complete", "error"};
	enum clm_agent_state s = clm_agent_get_state(check_agent(L, 1)->agent);

	lua_pushstring(L, (unsigned)s < 6 ? names[s] : "unknown");
	return 1;
}

static int
l_error(lua_State *L)
{
	lua_pushstring(L, clm_agent_get_last_error(check_agent(L, 1)->agent));
	return 1;
}

static int
l_close(lua_State *L)
{
	struct lagent *la = luaL_checkudata(L, 1, AGENT_META);

	agent_close(la);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Module functions                                                    */
/* ------------------------------------------------------------------ */

struct sleeper {
	uv_timer_t timer;
	struct wait w;
};

static void
sleeper_closed(uv_handle_t *h)
{
	free(h->data);
}

static void
sleeper_fire(uv_timer_t *t)
{
	struct sleeper *s = t->data;

	uv_close((uv_handle_t *)t, sleeper_closed);
	wait_end(&s->w, 0);
}

static int
l_sleep_k(lua_State *L, int status, lua_KContext ctx)
{
	(void)L;
	(void)status;
	(void)ctx;
	return 0;
}

/* clm.sleep(ms), from inside a coroutine. */
static int
l_sleep(lua_State *L)
{
	lua_Integer ms = luaL_checkinteger(L, 1);
	struct sleeper *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return luaL_error(L, "clm.sleep: out of memory");
	if (wait_begin(L, &s->w) < 0) {
		free(s);
		return luaL_error(L, "clm.sleep: call it inside clm.run");
	}
	uv_timer_init(&loop, &s->timer);
	s->timer.data = s;
	uv_timer_start(&s->timer, sleeper_fire, ms > 0 ? (uint64_t)ms : 0, 0);
	return lua_yieldk(L, 0, 0, l_sleep_k);
}

/* One HTTP host for clm.post, made on first use. */
static struct clm_host *post_host;

struct post {
	struct wait w;
	bool starting, done;
	int status;
	char *body;
	char err[256];
};

static int
post_push(lua_State *L, struct post *p)
{
	int n;

	if (p->err[0] != '\0') {
		lua_pushnil(L);
		lua_pushstring(L, p->err);
		n = 2;
	} else {
		lua_pushinteger(L, p->status);
		lua_pushstring(L, p->body != NULL ? p->body : "");
		n = 2;
	}
	free(p->body);
	free(p);
	return n;
}

static void
post_wake(struct post *p)
{
	p->done = true;
	if (p->starting)
		return; /* clm.post returns the result itself */
	{
		struct wait w = p->w;
		int n = post_push(mainL, p);

		lua_xmove(mainL, w.co, n);
		wait_end(&w, n);
	}
}

static void
post_ok(struct clm_http_response *resp, void *user)
{
	struct post *p = user;

	p->status = resp->status_code;
	p->body = resp->body;
	resp->body = NULL;
	free(resp->error_msg);
	resp->error_msg = NULL;
	post_wake(p);
}

static void
post_fail(int code, const char *msg, void *user)
{
	struct post *p = user;

	(void)code;
	(void)snprintf(p->err, sizeof(p->err), "%s",
	    msg != NULL && msg[0] != '\0' ? msg : "request failed");
	post_wake(p);
}

static int
l_post_k(lua_State *L, int status, lua_KContext ctx)
{
	(void)status;
	(void)ctx;
	(void)L;
	return 2;
}

/* clm.post(url, body[, headers]) -> status, body | nil, err. Inside a
 * coroutine; JSON content type unless headers says otherwise. */
static int
l_post(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checkstring(L, 2);
	char *hdrs[16] = {NULL};
	struct clm_http_req req = {0};
	struct post *p = NULL;
	int n = 0, r;

	if (lua_istable(L, 3)) {
		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			if (n < 15 && lua_type(L, -2) == LUA_TSTRING &&
			    lua_isstring(L, -1) &&
			    asprintf(&hdrs[n], "%s: %s", lua_tostring(L, -2),
			        lua_tostring(L, -1)) >= 0)
				n++;
			lua_pop(L, 1);
		}
	} else {
		hdrs[n++] = strdup("Content-Type: application/json");
	}
	if (post_host == NULL && clm_host_uv_new(&loop, &post_host) < 0)
		r = -ENOMEM;
	else if ((p = calloc(1, sizeof(*p))) == NULL)
		r = -ENOMEM;
	else if (wait_begin(L, &p->w) < 0) {
		free(p);
		r = -EAGAIN;
	} else {
		req.url = url;
		req.body = body;
		req.headers = (const char *const *)hdrs;
		p->starting = true;
		r = post_host->http_post(
		    post_host->ctx, &req, post_ok, post_fail, NULL, p, NULL);
		p->starting = false;
	}
	for (int i = 0; i < n; i++)
		free(hdrs[i]);
	if (r == -EAGAIN)
		return luaL_error(L, "clm.post: call it inside clm.run");
	if (r == -ENOMEM && p == NULL)
		return luaL_error(L, "clm.post: out of memory");
	if (r < 0) {
		luaL_unref(mainL, LUA_REGISTRYINDEX, p->w.ref);
		free(p);
		lua_pushnil(L);
		lua_pushstring(L, strerror(-r));
		return 2;
	}
	if (p->done) {
		luaL_unref(mainL, LUA_REGISTRYINDEX, p->w.ref);
		return post_push(L, p);
	}
	return lua_yieldk(L, 0, 0, l_post_k);
}

/* clm.spawn(fn, ...) runs fn in a new coroutine; errors are printed. */
static int
l_spawn(lua_State *L)
{
	int n = lua_gettop(L);

	luaL_checktype(L, 1, LUA_TFUNCTION);
	(void)task_start(L, n - 1, NULL, NULL, false);
	return 0;
}

/* clm.run(fn, ...): run fn in a coroutine, driving the loop until it
 * returns. Raises fn's error. */
static int
l_run(lua_State *L)
{
	int n = lua_gettop(L);
	struct task *t;
	char *err;
	bool ok;

	luaL_checktype(L, 1, LUA_TFUNCTION);
	t = task_start(L, n - 1, NULL, NULL, true);
	if (t == NULL)
		return luaL_error(L, "clm.run: out of memory");
	while (!t->done)
		uv_run(&loop, UV_RUN_ONCE);
	ok = t->ok;
	err = t->err;
	free(t);
	if (!ok) {
		lua_pushstring(L, err != NULL ? err : "error");
		free(err);
		return lua_error(L);
	}
	return 0;
}

/* clm.step(): run ready loop work once, without waiting. */
static int
l_step(lua_State *L)
{
	(void)L;
	uv_run(&loop, UV_RUN_NOWAIT);
	return 0;
}

static const luaL_Reg agent_methods[] = {
    {"turn", l_turn},
    {"notify", l_notify},
    {"cancel", l_cancel},
    {"tool", l_tool},
    {"on", l_on},
    {"prompt_set", l_prompt_set},
    {"history", l_history},
    {"clear", l_clear},
    {"state", l_state},
    {"error", l_error},
    {"close", l_close},
    {NULL, NULL},
};

static const luaL_Reg module_fns[] = {
    {"agent", l_agent},
    {"run", l_run},
    {"spawn", l_spawn},
    {"sleep", l_sleep},
    {"post", l_post},
    {"step", l_step},
    {NULL, NULL},
};

int luaopen_clm(lua_State *L);

int
luaopen_clm(lua_State *L)
{
	if (mainL == NULL) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		mainL = lua_tothread(L, -1);
		lua_pop(L, 1);
		uv_loop_init(&loop);
	}
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, TASKS_KEY);
	if (luaL_newmetatable(L, AGENT_META)) {
		lua_newtable(L);
		luaL_setfuncs(L, agent_methods, 0);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, l_close);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);
	luaL_newlib(L, module_fns);
	return 1;
}
