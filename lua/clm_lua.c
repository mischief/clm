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
#include <sys/queue.h>

#include <cjson/cJSON.h>
#include <lauxlib.h>
#include <lua.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/history.h"
#include "clm/host_uv.h"
#include "proc.h"

#define AGENT_META "clm.agent"
#define CTX_META "clm.ctx"
#define CTX_KEY "clm.context"
#define TASKS_KEY "clm.tasks"
#define AGENTS_KEY "clm.agents"

cJSON *clm_lua_to_cjson(lua_State *L, int idx);
void clm_lua_push_json_value(lua_State *L, cJSON *obj);

/*
 * One per Lua state that loads the module, kept in its registry: the loop
 * all its agents share, the state's main thread for callbacks, and the
 * HTTP host for clm.post. Nothing is shared between Lua states.
 */
struct cctx {
	uv_loop_t loop;
	lua_State *L;
	struct clm_host *post_host;
	int in_loop; /* uv_run calls under way; uv_run must not nest */
	TAILQ_HEAD(, lexec) execs; /* clm.exec calls still running */
};

static struct cctx *
ctx_of(lua_State *L)
{
	struct cctx *c;

	lua_getfield(L, LUA_REGISTRYINDEX, CTX_KEY);
	c = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return c;
}

static void
run_loop(struct cctx *c, uv_run_mode mode)
{
	c->in_loop++;
	uv_run(&c->loop, mode);
	c->in_loop--;
}

/* ------------------------------------------------------------------ */
/* Tasks: coroutines this module starts, with a hook for their end     */
/* ------------------------------------------------------------------ */

struct task {
	struct cctx *c;
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
task_of(struct cctx *c, lua_State *co)
{
	lua_State *mainL = c->L;
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
	lua_State *mainL = t->c->L;

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
co_resume(struct cctx *c, lua_State *co, int nargs)
{
	struct task *t = task_of(c, co);
	int nres = 0, rc = lua_resume(co, c->L, nargs, &nres);

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
task_start(struct cctx *c, lua_State *L, int nargs,
    void (*finish)(struct task *, bool, int), void *user, bool keep)
{
	lua_State *mainL = c->L;
	struct task *t = calloc(1, sizeof(*t));

	if (t == NULL) {
		lua_pop(L, nargs + 1);
		return NULL;
	}
	t->c = c;
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
	co_resume(c, t->co, nargs);
	return t;
}

/* A coroutine parked until a callback resumes it. */
struct wait {
	struct cctx *c;
	lua_State *co;
	int ref;
};

static int
wait_begin(struct cctx *c, lua_State *L, struct wait *w)
{
	if (L == c->L || !lua_isyieldable(L))
		return -1;
	w->c = c;
	w->co = L;
	lua_pushthread(L);
	lua_xmove(L, c->L, 1);
	w->ref = luaL_ref(c->L, LUA_REGISTRYINDEX);
	return 0;
}

static void
wait_end(struct wait *w, int nargs)
{
	struct cctx *c = w->c;
	lua_State *co = w->co;

	luaL_unref(c->L, LUA_REGISTRYINDEX, w->ref);
	w->co = NULL;
	co_resume(c, co, nargs);
}

/* ------------------------------------------------------------------ */
/* Agents                                                              */
/* ------------------------------------------------------------------ */

#define MAX_STRS 12

/*
 * The Lua values an agent needs (its options and callbacks, hooks and tool
 * functions) live in its userdata's user value, never in the registry, so
 * a callback that captures the agent forms a cycle the collector can free.
 * AGENTS_KEY maps each agent to its userdata, weakly, for C callbacks.
 */
struct lagent {
	struct cctx *c;
	struct clm_agent *agent;
	struct clm_host *host;
	char *strs[MAX_STRS];
	size_t nstrs;
	bool busy;
	int status;
	char *text;
	struct wait waiter;
	bool sync_wait;
};

/* Push the agent's store table, {opts, hooks, tools}, on L. */
static void
push_store(lua_State *L, struct lagent *la)
{
	lua_getfield(L, LUA_REGISTRYINDEX, AGENTS_KEY);
	if (lua_rawgetp(L, -1, la) == LUA_TUSERDATA)
		lua_getiuservalue(L, -1, 1);
	else
		lua_newtable(L); /* being collected: nothing to call */
	lua_replace(L, -3);
	lua_pop(L, 1);
}

/* Push store[field] on L. */
static void
push_field(lua_State *L, struct lagent *la, const char *field)
{
	push_store(L, la);
	lua_getfield(L, -1, field);
	lua_remove(L, -2);
}

static struct lagent *
check_agent(lua_State *L, int idx)
{
	struct lagent *la = luaL_checkudata(L, idx, AGENT_META);

	if (la->agent == NULL)
		luaL_error(L, "clm: agent is closed");
	return la;
}

/* Push opts[name] on the main thread; true if it is a function. */
static bool
push_cb(struct lagent *la, const char *name)
{
	lua_State *mainL = la->c->L;

	push_field(mainL, la, "opts");
	lua_getfield(mainL, -1, name);
	lua_remove(mainL, -2);
	if (lua_isfunction(mainL, -1))
		return true;
	lua_pop(mainL, 1);
	return false;
}

static void
call_cb(lua_State *mainL, const char *name, int nargs)
{
	if (lua_pcall(mainL, nargs, 0, 0) != LUA_OK) {
		report(name, lua_tostring(mainL, -1));
		lua_pop(mainL, 1);
	}
}

static void
cb_text(const char *text, void *user)
{
	lua_State *mainL = ((struct lagent *)user)->c->L;

	if (!push_cb(user, "on_text"))
		return;
	lua_pushstring(mainL, text);
	call_cb(mainL, "on_text", 1);
}

static void
cb_reasoning(const char *text, void *user)
{
	lua_State *mainL = ((struct lagent *)user)->c->L;

	if (!push_cb(user, "on_reasoning"))
		return;
	lua_pushstring(mainL, text);
	call_cb(mainL, "on_reasoning", 1);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	lua_State *mainL = ((struct lagent *)user)->c->L;

	if (!push_cb(user, "on_tool_begin"))
		return;
	lua_pushstring(mainL, name);
	lua_pushstring(mainL, args != NULL ? args : "{}");
	call_cb(mainL, "on_tool_begin", 2);
}

static void
cb_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	static const char *const names[] = {"ok", "failed", "timedout"};
	lua_State *mainL = ((struct lagent *)user)->c->L;

	if (!push_cb(user, "on_tool"))
		return;
	lua_pushstring(mainL, name);
	lua_pushstring(mainL, content != NULL ? content : "");
	lua_pushstring(
	    mainL, (unsigned)outcome < 3 ? names[outcome] : "failed");
	call_cb(mainL, "on_tool", 3);
}

static void
cb_notice(const char *text, void *user)
{
	lua_State *mainL = ((struct lagent *)user)->c->L;

	if (!push_cb(user, "on_notice"))
		return;
	lua_pushstring(mainL, text);
	call_cb(mainL, "on_notice", 1);
}

static void
cb_usage(const struct clm_usage *u, void *user)
{
	lua_State *mainL = ((struct lagent *)user)->c->L;

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
	call_cb(mainL, "on_usage", 1);
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct lagent *la = user;
	lua_State *mainL = la->c->L;
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
		push_field(mainL, la, "opts");
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
	lua_State *mainL = la->c->L;

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
	lua_State *mainL = la->c->L;

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
	lua_State *mainL = la->c->L;
	const char *ev =
	    info->event == CLM_TURN_START ? "turn_start" : "turn_end";
	int n, i;

	if (info->event == CLM_TURN_END) {
		free(la->text);
		la->text = info->text != NULL ? strdup(info->text) : NULL;
	}
	push_field(mainL, la, "hooks");
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
		call_cb(mainL, ev, 1);
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
	lua_State *mainL = la->c->L;
	struct gate_task *g = calloc(1, sizeof(*g));
	cJSON *args;

	if (g == NULL) {
		(void)clm_tool_gate_respond(gate, CLM_GATE_PASS, NULL, NULL);
		return;
	}
	g->gate = gate;
	push_field(mainL, la, "hooks");
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
	(void)task_start(la->c, mainL, 1, gate_finish, g, false);
}

/* Tools written in Lua: invoke(args) returns the result, or raises. */
struct ltool {
	struct lagent *la;
	char *name;
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
	lua_State *mainL = lt->la->c->L;
	cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));

	push_field(mainL, lt->la, "tools");
	lua_getfield(mainL, -1, lt->name);
	lua_remove(mainL, -2);
	if (args != NULL) {
		clm_lua_push_json_value(mainL, args);
		cJSON_Delete(args);
	} else {
		lua_newtable(mainL);
	}
	if (task_start(lt->la->c, mainL, 1, tool_finish, inv, false) == NULL)
		clm_tool_fail(inv, "out of memory");
}

static void
tool_detach(void *user)
{
	struct ltool *lt = user;

	free(lt->name);
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
	lua_State *mainL = la->c != NULL ? la->c->L : NULL;

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
}

/* clm.agent{url=, model=, provider=, api_key=, system_prompt=, stream=,
 * effort=, compact_effort=, context_size=, max_iterations=,
 * tools={"shell",...}, permission="allow"|"deny"|fn, on_text=fn, ...} */
static int
l_agent(lua_State *L)
{
	struct lagent *la;
	struct clm_cfg cfg = {0};
	const char *url, *kind, *effort, *compact_effort;
	char endpoint[512];
	int r;

	luaL_checktype(L, 1, LUA_TTABLE);
	la = lua_newuserdatauv(L, sizeof(*la), 1);
	memset(la, 0, sizeof(*la));
	la->c = ctx_of(L);
	luaL_setmetatable(L, AGENT_META);
	lua_createtable(L, 0, 3);
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "opts");
	lua_newtable(L);
	lua_setfield(L, -2, "hooks");
	lua_newtable(L);
	lua_setfield(L, -2, "tools");
	lua_setiuservalue(L, -2, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, AGENTS_KEY);
	lua_pushvalue(L, -2);
	lua_rawsetp(L, -2, la);
	lua_pop(L, 1);

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
	compact_effort = keep_str(la, L, 1, "compact_effort");
	lua_getfield(L, 1, "stream");
	cfg.stream = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
	lua_pop(L, 1);
	cfg.context_size = opt_int(L, 1, "context_size", 0);
	cfg.max_iterations = (size_t)opt_int(L, 1, "max_iterations", 0);

	r = clm_host_uv_new(&la->c->loop, &la->host);
	if (r == 0)
		r = clm_agent_new(&cfg, la->host, &callbacks, la, &la->agent);
	if (r < 0) {
		agent_close(la);
		return luaL_error(L, "clm.agent: %s", strerror(-r));
	}
	if (effort != NULL)
		(void)clm_agent_set_effort(la->agent, effort);
	(void)clm_agent_set_compact_effort(la->agent, compact_effort);
	lua_getfield(L, 1, "cache_system");
	clm_agent_set_cache_system(la->agent, lua_toboolean(L, -1));
	lua_pop(L, 1);
	(void)clm_agent_add_turn_hook(la->agent, turn_hook, la);

	/* builtins = false drops the file tools; a list keeps only those. */
	lua_getfield(L, 1, "builtins");
	if (lua_isboolean(L, -1) || lua_istable(L, -1)) {
		static const char *const names[] = {
		    "read_file", "write_file", "list_dir", "read_image"};

		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			bool keep = false;

			if (lua_istable(L, -1)) {
				for (int j = 1; j <= (int)lua_rawlen(L, -1);
				    j++) {
					const char *s;

					lua_rawgeti(L, -1, j);
					s = lua_tostring(L, -1);
					keep = keep ||
					    (s && strcmp(s, names[i]) == 0);
					lua_pop(L, 1);
				}
			} else {
				keep = lua_toboolean(L, -1);
			}
			if (!keep)
				(void)clm_tool_remove(la->agent, names[i]);
		}
	}
	lua_pop(L, 1);

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
	if (wait_begin(la->c, L, &la->waiter) < 0 && la->c->in_loop > 0)
		return luaL_error(L,
		    "clm: turn outside a coroutine while the "
		    "loop runs; call it from clm.run's function");
	free(la->text);
	la->text = NULL;
	la->busy = true;
	r = clm_agent_submit(la->agent, prompt);
	if (r < 0) {
		if (la->waiter.co != NULL) {
			luaL_unref(la->c->L, LUA_REGISTRYINDEX, la->waiter.ref);
			la->waiter.co = NULL;
		}
		la->busy = false;
		la->status = r;
		push_result(la);
		lua_xmove(la->c->L, L, 3);
		return 3;
	}
	if (la->waiter.co != NULL)
		return lua_yieldk(L, 0, 0, l_turn_k);
	while (la->busy)
		run_loop(la->c, UV_RUN_ONCE);
	push_result(la);
	lua_xmove(la->c->L, L, 3);
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
	lt = calloc(1, sizeof(*lt));
	if (lt == NULL || (lt->name = strdup(def.name)) == NULL) {
		free(lt);
		free(schema);
		return luaL_error(L, "clm: out of memory");
	}
	lt->la = la;
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
	lua_getiuservalue(L, 1, 1);
	lua_getfield(L, -1, "tools");
	lua_pushvalue(L, -3); /* the invoke function */
	lua_setfield(L, -2, def.name);
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
	lua_getiuservalue(L, 1, 1);
	lua_getfield(L, -1, "hooks");
	lua_remove(L, -2);
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
	if (wait_begin(ctx_of(L), L, &s->w) < 0) {
		free(s);
		return luaL_error(L, "clm.sleep: call it inside clm.run");
	}
	uv_timer_init(&s->w.c->loop, &s->timer);
	s->timer.data = s;
	uv_timer_start(&s->timer, sleeper_fire, ms > 0 ? (uint64_t)ms : 0, 0);
	return lua_yieldk(L, 0, 0, l_sleep_k);
}

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
		int n = post_push(w.c->L, p);

		lua_xmove(w.c->L, w.co, n);
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
	struct cctx *c = ctx_of(L);
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
	if (c->post_host == NULL &&
	    clm_host_uv_new(&c->loop, &c->post_host) < 0)
		r = -ENOMEM;
	else if ((p = calloc(1, sizeof(*p))) == NULL)
		r = -ENOMEM;
	else if (wait_begin(c, L, &p->w) < 0) {
		free(p);
		r = -EAGAIN;
	} else {
		req.url = url;
		req.body = body;
		req.headers = (const char *const *)hdrs;
		p->starting = true;
		r = c->post_host->http_post(
		    c->post_host->ctx, &req, post_ok, post_fail, NULL, p, NULL);
		p->starting = false;
	}
	for (int i = 0; i < n; i++)
		free(hdrs[i]);
	if (r == -EAGAIN)
		return luaL_error(L, "clm.post: call it inside clm.run");
	if (r == -ENOMEM && p == NULL)
		return luaL_error(L, "clm.post: out of memory");
	if (r < 0) {
		luaL_unref(c->L, LUA_REGISTRYINDEX, p->w.ref);
		free(p);
		lua_pushnil(L);
		lua_pushstring(L, strerror(-r));
		return 2;
	}
	if (p->done) {
		luaL_unref(c->L, LUA_REGISTRYINDEX, p->w.ref);
		return post_push(L, p);
	}
	return lua_yieldk(L, 0, 0, l_post_k);
}

/* ------------------------------------------------------------------ */
/* clm.exec: a shell command, without blocking the loop                */
/* ------------------------------------------------------------------ */

#define EXEC_TIMEOUT_MS 120000

/* A clm.exec call in flight, on its context's list until it ends. */
struct lexec {
	struct wait w;
	struct clm_proc *p;
	TAILQ_ENTRY(lexec) entries;
};

/* Every handle is closed: resume the caller with code, output, why. */
static void
exec_done(struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct lexec *x = user;
	struct cctx *c = x->w.c;
	lua_State *mainL = c->L;
	struct wait w = x->w;

	(void)p;
	TAILQ_REMOVE(&c->execs, x, entries);
	free(x);
	if (r->timed_out || r->term_signal != 0)
		lua_pushnil(mainL);
	else
		lua_pushinteger(mainL, (lua_Integer)r->exit_status);
	lua_pushlstring(mainL, r->output, r->len);
	if (r->dropped > 0) {
		lua_pushfstring(mainL,
		    "\n[%I more bytes of output were not kept]\n",
		    (lua_Integer)r->dropped);
		lua_concat(mainL, 2);
	}
	if (r->timed_out)
		lua_pushliteral(mainL, "timed out");
	else if (r->term_signal != 0)
		lua_pushfstring(mainL, "killed by signal %d", r->term_signal);
	else
		lua_pushnil(mainL);
	lua_xmove(mainL, w.co, 3);
	wait_end(&w, 3);
}

static int
l_exec_k(lua_State *L, int status, lua_KContext ctx)
{
	(void)L;
	(void)status;
	(void)ctx;
	return 3;
}

/* clm.exec(cmd[, opts]) -> code, output | nil, output, why. Runs cmd with
 * /bin/sh -c inside a coroutine. opts: cwd, timeout_ms (default 120000; 0
 * for none), max (output bytes kept, default 1 MiB), stdin (a string).
 * stdout and stderr are kept together, in the order they arrive. */
static int
l_exec(lua_State *L)
{
	struct clm_proc_opts o = {
	    .command = luaL_checkstring(L, 1),
	    .shell = "/bin/sh",
	    .timeout_ms = EXEC_TIMEOUT_MS,
	    .exit_grace_ms = clm_proc_grace_ms(),
	    .done = exec_done,
	};
	struct cctx *c = ctx_of(L);
	struct lexec *x;
	int r;

	if (lua_istable(L, 2)) {
		/* cwd and stdin stay on the stack until the spawn has copied
		 * them. */
		lua_getfield(L, 2, "cwd");
		o.cwd = lua_tostring(L, -1);
		lua_getfield(L, 2, "stdin");
		o.stdin_data = lua_tostring(L, -1);
		lua_getfield(L, 2, "timeout_ms");
		if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0)
			o.timeout_ms = (uint64_t)lua_tointeger(L, -1);
		lua_getfield(L, 2, "max");
		if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0)
			o.max = (size_t)lua_tointeger(L, -1);
		lua_pop(L, 2);
	}
	if ((x = calloc(1, sizeof(*x))) == NULL)
		return luaL_error(L, "clm.exec: out of memory");
	if (wait_begin(c, L, &x->w) < 0) {
		free(x);
		return luaL_error(L, "clm.exec: call it inside clm.run");
	}
	o.user = x;
	r = clm_proc_spawn(&c->loop, &o, &x->p);
	if (r < 0) {
		luaL_unref(c->L, LUA_REGISTRYINDEX, x->w.ref);
		free(x);
		lua_pushnil(L);
		lua_pushliteral(L, "");
		lua_pushstring(L, uv_strerror(r));
		return 3;
	}
	TAILQ_INSERT_TAIL(&c->execs, x, entries);
	return lua_yieldk(L, 0, 0, l_exec_k);
}

/* clm.spawn(fn, ...) runs fn in a new coroutine; errors are printed. */
static int
l_spawn(lua_State *L)
{
	int n = lua_gettop(L);

	luaL_checktype(L, 1, LUA_TFUNCTION);
	(void)task_start(ctx_of(L), L, n - 1, NULL, NULL, false);
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

	struct cctx *c = ctx_of(L);

	luaL_checktype(L, 1, LUA_TFUNCTION);
	if (c->in_loop > 0)
		return luaL_error(L, "clm.run: the loop is already running");
	/* Busy from the first resume on, so fn cannot start a second run. */
	c->in_loop++;
	t = task_start(c, L, n - 1, NULL, NULL, true);
	if (t == NULL) {
		c->in_loop--;
		return luaL_error(L, "clm.run: out of memory");
	}
	while (!t->done)
		uv_run(&c->loop, UV_RUN_ONCE);
	c->in_loop--;
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
	struct cctx *c = ctx_of(L);

	if (c->in_loop > 0)
		return luaL_error(L, "clm.step: the loop is already running");
	run_loop(c, UV_RUN_NOWAIT);
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
    {"exec", l_exec},
    {"step", l_step},
    {NULL, NULL},
};

static void
close_handle(uv_handle_t *h, void *arg)
{
	(void)arg;
	if (!uv_is_closing(h))
		uv_close(h, NULL);
}

/* The state is closing: its agents went first, so only the loop is left. */
static int
ctx_gc(lua_State *L)
{
	struct cctx *c = luaL_checkudata(L, 1, CTX_META);
	struct lexec *x;

	if (c->L == NULL)
		return 0;
	/* clm.exec calls end with the state: their coroutines are gone, and
	 * their commands must not outlive it. */
	while ((x = TAILQ_FIRST(&c->execs)) != NULL) {
		TAILQ_REMOVE(&c->execs, x, entries);
		clm_proc_detach(x->p);
		clm_proc_kill(x->p);
		free(x);
	}
	if (c->post_host != NULL) {
		clm_host_uv_free(c->post_host);
		c->post_host = NULL;
	}
	uv_walk(&c->loop, close_handle, NULL);
	uv_run(&c->loop, UV_RUN_DEFAULT);
	(void)uv_loop_close(&c->loop);
	c->L = NULL;
	return 0;
}

int luaopen_clm(lua_State *L);

int
luaopen_clm(lua_State *L)
{
	/* Made before any agent, so the state closes the agents first. */
	if (ctx_of(L) == NULL) {
		struct cctx *c = lua_newuserdatauv(L, sizeof(*c), 0);

		memset(c, 0, sizeof(*c));
		TAILQ_INIT(&c->execs);
		if (uv_loop_init(&c->loop) != 0)
			return luaL_error(L, "clm: cannot make an event loop");
		lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		c->L = lua_tothread(L, -1);
		lua_pop(L, 1);
		luaL_newmetatable(L, CTX_META);
		lua_pushcfunction(L, ctx_gc);
		lua_setfield(L, -2, "__gc");
		lua_setmetatable(L, -2);
		lua_setfield(L, LUA_REGISTRYINDEX, CTX_KEY);
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, TASKS_KEY);
		lua_newtable(L);
		lua_createtable(L, 0, 1);
		lua_pushstring(L, "v");
		lua_setfield(L, -2, "__mode");
		lua_setmetatable(L, -2);
		lua_setfield(L, LUA_REGISTRYINDEX, AGENTS_KEY);
	}
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
