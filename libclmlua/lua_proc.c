// SPDX-License-Identifier: ISC
/*
 * clm.spawn, clm.exec, clm.after, clm.notify and clm.getenv for plugins.
 * Children run through the host's proc_spawn and timers through its
 * timer_set, so this file needs no event loop of its own.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/log.h"
#include "lua_internal.h"
#include "banned.h"

#define CLM_LUA_PROC_META "clm_proc"
#define CLM_LUA_TIMER_META "clm_timer"

#define CLM_LUA_ARGV_MAX 256
#define CLM_LUA_LINE_MAX (64 * 1024)        /* longer lines are split */
#define CLM_LUA_EXEC_OUT_MAX (1024 * 1024)  /* per stream, for clm.exec */
#define CLM_LUA_STDERR_DROP_MAX (64 * 1024) /* kept for a spawn error */

void clm_lua_mark_invocation_thread(lua_State *L, lua_State *co, int on);
int clm_lua_is_invocation_thread(lua_State *L);
void clm_lua_clear_invocation_registry(lua_State *L);
int clm_lua_resume_with_deadline(struct clm_lua_plugin *plugin, lua_State *co,
    lua_State *from, int nargs, int *nresults, uint64_t timeout_ms);

struct lbuf {
	char *buf;
	size_t len, cap;
	bool truncated;
};

static int
lbuf_add(struct lbuf *b, const char *s, size_t n, size_t max)
{
	if (b->len + n > max) {
		n = b->len < max ? max - b->len : 0;
		b->truncated = true;
	}
	if (n == 0)
		return 0;
	if (b->len + n > b->cap) {
		size_t ncap = b->cap ? b->cap : 256;
		char *nb;

		while (ncap < b->len + n)
			ncap *= 2;
		nb = realloc(b->buf, ncap);
		if (nb == NULL)
			return -ENOMEM;
		b->buf = nb;
		b->cap = ncap;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	return 0;
}

static struct clm_lua_plugin *
get_plugin(lua_State *L)
{
	struct clm_lua_plugin *p;

	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_plugin");
	p = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return p;
}

static struct clm_host *
get_host(const struct clm_lua_plugin *plugin)
{
	struct clm_agent *agent = clm_lua_plugin_agent(plugin);

	return agent != NULL ? agent->host : NULL;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

struct lua_proc_ud {
	struct lua_proc *p; /* NULL once the child is gone */
};

struct lua_proc {
	struct clm_lua_pending pending;
	struct clm_lua_plugin *plugin;
	struct clm_host *host;
	struct clm_proc *proc;
	struct lua_proc_ud *ud;
	int ud_ref; /* keeps the handle alive while the child runs */
	int on_line_ref, on_stderr_ref, on_exit_ref;
	struct lbuf out, err; /* spawn: partial line; exec: whole output */

	/* clm.exec only: the yielded tool coroutine. */
	bool exec;
	lua_State *co;
	int co_ref;
	struct clm_tool_invocation *inv;
	uint64_t timeout_ms;
};

static void
lua_proc_unref(lua_State *L, int *ref)
{
	if (*ref != LUA_NOREF && *ref != LUA_REFNIL)
		luaL_unref(L, LUA_REGISTRYINDEX, *ref);
	*ref = LUA_NOREF;
}

/* Frees the C side. Lua is only touched when lua is true. */
static void
lua_proc_free(struct lua_proc *p, bool lua)
{
	if (p->ud != NULL)
		p->ud->p = NULL;
	if (lua && clm_lua_plugin_alive(p->plugin)) {
		lua_State *L = clm_lua_plugin_state(p->plugin);

		lua_proc_unref(L, &p->ud_ref);
		lua_proc_unref(L, &p->on_line_ref);
		lua_proc_unref(L, &p->on_stderr_ref);
		lua_proc_unref(L, &p->on_exit_ref);
	}
	free(p->out.buf);
	free(p->err.buf);
	free(p);
}

struct lua_line_arg {
	int ref;
	const char *s;
	size_t n;
};

static int
lua_push_line(lua_State *L, void *arg)
{
	struct lua_line_arg *a = arg;

	lua_rawgeti(L, LUA_REGISTRYINDEX, a->ref);
	lua_pushlstring(L, a->s, a->n);
	return 1;
}

static void
lua_proc_line(struct lua_proc *p, int ref, const char *s, size_t n)
{
	struct lua_line_arg a = {ref, s, n};

	if (n > 0 && s[n - 1] == '\r')
		a.n--;
	(void)clm_lua_plugin_callback(p->plugin, lua_push_line, &a);
}

/* Split b into lines for ref; keep the unfinished tail. */
static void
lua_proc_lines(struct lua_proc *p, struct lbuf *b, int ref, bool final)
{
	size_t start = 0;

	if (b->len == 0)
		return;
	for (size_t i = 0; i < b->len; i++) {
		if (b->buf[i] != '\n')
			continue;
		lua_proc_line(p, ref, b->buf + start, i - start);
		start = i + 1;
	}
	if (final || b->len - start >= CLM_LUA_LINE_MAX) {
		if (b->len > start)
			lua_proc_line(p, ref, b->buf + start, b->len - start);
		start = b->len;
	}
	memmove(b->buf, b->buf + start, b->len - start);
	b->len -= start;
}

static void
lua_proc_data(int fd, const char *data, size_t len, void *user)
{
	struct lua_proc *p = user;
	struct lbuf *b = fd == 1 ? &p->out : &p->err;
	int ref = fd == 1 ? p->on_line_ref : p->on_stderr_ref;

	if (p->exec) {
		(void)lbuf_add(b, data, len, CLM_LUA_EXEC_OUT_MAX);
		return;
	}
	if (ref == LUA_NOREF) {
		/* Keep the start of an unread stderr for the exit message. */
		(void)lbuf_add(b, data, len, CLM_LUA_STDERR_DROP_MAX);
		return;
	}
	if (lbuf_add(b, data, len, SIZE_MAX) == 0)
		lua_proc_lines(p, b, ref, false);
}

struct lua_exit_arg {
	struct lua_proc *p;
	int64_t status;
	int signal;
};

static int
lua_push_exit(lua_State *L, void *arg)
{
	struct lua_exit_arg *a = arg;

	lua_rawgeti(L, LUA_REGISTRYINDEX, a->p->on_exit_ref);
	if (a->signal != 0)
		lua_pushnil(L);
	else
		lua_pushinteger(L, (lua_Integer)a->status);
	lua_pushinteger(L, a->signal);
	if (a->p->on_stderr_ref == LUA_NOREF && a->p->err.len > 0)
		lua_pushlstring(L, a->p->err.buf, a->p->err.len);
	else
		lua_pushnil(L);
	return 3;
}

static void lua_exec_done(struct lua_proc *p, int64_t status, int sig);

static void
lua_proc_exit(int64_t status, int sig, void *user)
{
	struct lua_proc *p = user;

	(void)clm_lua_pending_remove(&p->pending);
	p->proc = NULL;
	if (p->ud != NULL)
		p->ud->p = NULL;
	if (p->exec) {
		lua_exec_done(p, status, sig);
		return;
	}
	if (p->on_line_ref != LUA_NOREF)
		lua_proc_lines(p, &p->out, p->on_line_ref, true);
	if (p->on_stderr_ref != LUA_NOREF)
		lua_proc_lines(p, &p->err, p->on_stderr_ref, true);
	if (p->on_exit_ref != LUA_NOREF) {
		struct lua_exit_arg a = {p, status, sig};

		(void)clm_lua_plugin_callback(p->plugin, lua_push_exit, &a);
	}
	lua_proc_free(p, true);
}

/* The Lua state is about to close: stop the child, touch no Lua. */
static void
lua_proc_teardown(struct clm_lua_pending *pending)
{
	struct lua_proc *p = (struct lua_proc *)pending;

	if (p->proc != NULL)
		p->host->proc_detach(p->proc);
	p->proc = NULL;
	if (p->exec && p->inv != NULL) {
		clm_tool_invocation_set_cancel(p->inv, NULL, NULL);
		clm_tool_fail(
		    p->inv, "lua plugin environment is shutting down");
	}
	lua_proc_free(p, false);
}

/* argv table at idx -> malloc'd NULL-terminated array borrowing the Lua
 * strings, which stay on the table while the caller spawns. */
static const char **
lua_check_argv(lua_State *L, int idx)
{
	const char **argv;
	lua_Integer n;

	luaL_checktype(L, idx, LUA_TTABLE);
	n = (lua_Integer)lua_rawlen(L, idx);
	if (n < 1 || n > CLM_LUA_ARGV_MAX)
		luaL_error(
		    L, "argv must hold 1 to %d strings", CLM_LUA_ARGV_MAX);
	argv = calloc((size_t)n + 1, sizeof(*argv));
	if (argv == NULL)
		luaL_error(L, "out of memory");
	for (lua_Integer i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, i);
		if (lua_type(L, -1) != LUA_TSTRING) {
			free(argv);
			luaL_error(L, "argv[%d] is not a string", (int)i);
		}
		argv[i - 1] = lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	return argv;
}

static int
lua_opt_fn(lua_State *L, int opts, const char *name)
{
	if (lua_isnoneornil(L, opts))
		return LUA_NOREF;
	lua_getfield(L, opts, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return LUA_NOREF;
	}
	if (!lua_isfunction(L, -1))
		luaL_error(L, "%s must be a function", name);
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Start the child for p. Raises on failure; p is freed first. */
static void
lua_proc_start(lua_State *L, struct lua_proc *p, int argv_idx, int opts)
{
	struct clm_proc_req req = {0};
	const char **argv;
	size_t slen = 0;
	int r;

	if (!lua_isnoneornil(L, opts)) {
		lua_getfield(L, opts, "stdin");
		if (lua_isstring(L, -1))
			req.stdin_data = lua_tolstring(L, -1, &slen);
		req.stdin_len = slen;
		/* Left on the stack so the string outlives the spawn. */
	}
	argv = lua_check_argv(L, argv_idx);
	req.argv = argv;
	r = p->host->proc_spawn(
	    p->host->ctx, &req, lua_proc_data, lua_proc_exit, p, &p->proc);
	free(argv);
	if (r < 0) {
		lua_proc_free(p, true);
		lua_rawgeti(L, argv_idx, 1);
		luaL_error(L, "%s: %s", lua_tostring(L, -1), strerror(-r));
	}
	(void)clm_lua_pending_add(p->plugin, &p->pending, lua_proc_teardown);
}

static struct lua_proc *
lua_proc_new(lua_State *L, struct clm_lua_plugin *plugin)
{
	struct clm_host *host = get_host(plugin);
	struct lua_proc *p;

	if (host == NULL || host->proc_spawn == NULL)
		luaL_error(L, "this host cannot start processes");
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		luaL_error(L, "out of memory");
	p->plugin = plugin;
	p->host = host;
	p->ud_ref = p->on_line_ref = p->on_stderr_ref = p->on_exit_ref =
	    LUA_NOREF;
	p->co_ref = LUA_NOREF;
	return p;
}

/*
 * clm.spawn(argv, opts) -> handle. opts: stdin (string), on_line(line),
 * on_stderr(line), on_exit(code, signal, stderr). code is nil when a signal
 * killed the child; stderr is the start of its output when no on_stderr.
 */
static int
lua_clm_spawn(lua_State *L)
{
	struct clm_lua_plugin *plugin = get_plugin(L);
	struct lua_proc_ud *ud;
	struct lua_proc *p;
	int line_ref, stderr_ref, exit_ref, ud_ref;

	luaL_checktype(L, 1, LUA_TTABLE);
	if (!lua_isnoneornil(L, 2))
		luaL_checktype(L, 2, LUA_TTABLE);
	lua_settop(L, 2);

	ud = lua_newuserdatauv(L, sizeof(*ud), 0); /* 3 */
	ud->p = NULL;
	luaL_setmetatable(L, CLM_LUA_PROC_META);

	/* Everything that can raise comes before p exists. */
	line_ref = lua_opt_fn(L, 2, "on_line");
	stderr_ref = lua_opt_fn(L, 2, "on_stderr");
	exit_ref = lua_opt_fn(L, 2, "on_exit");
	lua_pushvalue(L, 3);
	ud_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	p = lua_proc_new(L, plugin);
	p->on_line_ref = line_ref;
	p->on_stderr_ref = stderr_ref;
	p->on_exit_ref = exit_ref;
	p->ud_ref = ud_ref;
	p->ud = ud;
	ud->p = p;

	lua_proc_start(L, p, 1, 2);
	lua_pushvalue(L, 3);
	return 1;
}

static int
lua_signal_arg(lua_State *L, int idx)
{
	static const struct {
		const char *name;
		int sig;
	} names[] = {
	    {"TERM", SIGTERM},
	    {"KILL", SIGKILL},
	    {"INT", SIGINT},
	    {"HUP", SIGHUP},
	};

	if (lua_isnoneornil(L, idx))
		return SIGTERM;
	if (lua_isinteger(L, idx))
		return (int)lua_tointeger(L, idx);
	const char *s = luaL_checkstring(L, idx);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(s, names[i].name) == 0)
			return names[i].sig;
	return luaL_error(L, "unknown signal '%s'", s);
}

/* handle:kill([sig]) -> true, or false once the child is gone. */
static int
lua_proc_kill(lua_State *L)
{
	struct lua_proc_ud *ud = luaL_checkudata(L, 1, CLM_LUA_PROC_META);
	int sig = lua_signal_arg(L, 2);

	if (ud->p == NULL || ud->p->proc == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	ud->p->host->proc_kill(ud->p->proc, sig);
	lua_pushboolean(L, 1);
	return 1;
}

/* handle:running() -> bool */
static int
lua_proc_running(lua_State *L)
{
	struct lua_proc_ud *ud = luaL_checkudata(L, 1, CLM_LUA_PROC_META);

	lua_pushboolean(L, ud->p != NULL && ud->p->proc != NULL);
	return 1;
}

/* ------------------------------------------------------------------ */
/* clm.exec                                                            */
/* ------------------------------------------------------------------ */

static void
lua_exec_release(struct lua_proc *p)
{
	lua_State *L = clm_lua_plugin_state(p->plugin);

	clm_lua_mark_invocation_thread(L, p->co, 0);
	lua_proc_unref(L, &p->co_ref);
	clm_lua_clear_invocation_registry(L);
}

static void
lua_exec_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct lua_proc *p = user;

	if (clm_lua_pending_remove(&p->pending) == NULL)
		return;
	clm_tool_invocation_set_cancel(inv, NULL, NULL);
	if (p->proc != NULL)
		p->host->proc_detach(p->proc);
	p->proc = NULL;
	p->inv = NULL;
	if (clm_lua_plugin_alive(p->plugin))
		lua_exec_release(p);
	lua_proc_free(p, true);
	clm_tool_fail(inv, "clm.exec cancelled");
}

static void
lua_exec_push_buf(lua_State *co, const char *field, const struct lbuf *b)
{
	lua_pushlstring(co, b->buf != NULL ? b->buf : "", b->len);
	lua_setfield(co, -2, field);
}

static void
lua_exec_done(struct lua_proc *p, int64_t status, int sig)
{
	lua_State *L = clm_lua_plugin_state(p->plugin);
	lua_State *co = p->co;
	int nres = 0, rc;

	if (p->inv != NULL)
		clm_tool_invocation_set_cancel(p->inv, NULL, NULL);
	if (!clm_lua_plugin_alive(p->plugin)) {
		if (p->inv != NULL)
			clm_tool_fail(p->inv, "plugin state is unusable");
		lua_proc_free(p, false);
		return;
	}
	if (lua_status(co) != LUA_YIELD) {
		lua_exec_release(p);
		lua_proc_free(p, true);
		return;
	}

	lua_createtable(co, 0, 5);
	if (sig == 0) {
		lua_pushinteger(co, (lua_Integer)status);
		lua_setfield(co, -2, "code");
	}
	lua_pushinteger(co, sig);
	lua_setfield(co, -2, "signal");
	lua_exec_push_buf(co, "stdout", &p->out);
	lua_exec_push_buf(co, "stderr", &p->err);
	lua_pushboolean(co, p->out.truncated || p->err.truncated);
	lua_setfield(co, -2, "truncated");

	rc = clm_lua_resume_with_deadline(
	    p->plugin, co, L, 1, &nres, p->timeout_ms);
	if (rc != LUA_YIELD) {
		if (rc != LUA_OK) {
			const char *err = lua_tostring(co, -1);

			clm_debug("clm.exec: coroutine error on resume: %s",
			    err ? err : "(unknown)");
			if (p->inv != NULL)
				clm_tool_fail(
				    p->inv, err ? err : "lua runtime error");
		}
		lua_exec_release(p);
	}
	/* On LUA_YIELD the coroutine waits on something else, which now
	 * owns co_ref. */
	p->co_ref = LUA_NOREF;
	lua_proc_free(p, true);
}

/*
 * clm.exec(argv, opts) -> {code, signal, stdout, stderr, truncated}.
 * Yields the tool call until the child exits. opts: stdin (string).
 */
static int
lua_clm_exec(lua_State *L)
{
	struct clm_lua_plugin *plugin = get_plugin(L);
	struct lua_proc *p;

	if (!clm_lua_is_invocation_thread(L))
		return luaL_error(L,
		    "clm.exec may only be called from a "
		    "tool invocation coroutine");
	luaL_checktype(L, 1, LUA_TTABLE);
	if (!lua_isnoneornil(L, 2))
		luaL_checktype(L, 2, LUA_TTABLE);
	lua_settop(L, 2);

	p = lua_proc_new(L, plugin);
	p->exec = true;
	p->co = L;
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_co_ref");
	p->co_ref = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_inv");
	p->inv = lua_touserdata(L, -1);
	lua_pop(L, 1);
	p->timeout_ms = clm_tool_invocation_timeout_ms(p->inv);

	lua_proc_start(L, p, 1, 2);
	if (p->inv != NULL)
		clm_tool_invocation_set_cancel(p->inv, lua_exec_cancel, p);
	return lua_yield(L, 0);
}

/* ------------------------------------------------------------------ */
/* clm.after and clm.notify                                            */
/* ------------------------------------------------------------------ */

struct lua_timer_ud {
	struct lua_timer *t; /* NULL once fired or cancelled */
};

struct lua_timer {
	struct clm_lua_pending pending;
	struct clm_lua_plugin *plugin;
	struct clm_host *host;
	struct clm_timer *timer;
	struct lua_timer_ud *ud;
	int ud_ref, fn_ref;
	char *notify; /* clm.notify: text to deliver instead of fn */
};

static void
lua_timer_free(struct lua_timer *t, bool lua)
{
	if (t->timer != NULL)
		t->host->timer_cancel(t->timer);
	if (t->ud != NULL)
		t->ud->t = NULL;
	if (lua && clm_lua_plugin_alive(t->plugin)) {
		lua_State *L = clm_lua_plugin_state(t->plugin);

		lua_proc_unref(L, &t->ud_ref);
		lua_proc_unref(L, &t->fn_ref);
	}
	free(t->notify);
	free(t);
}

static int
lua_push_fn(lua_State *L, void *arg)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, *(int *)arg);
	return 0;
}

static void
lua_timer_fire(void *arg)
{
	struct lua_timer *t = arg;

	if (clm_lua_pending_remove(&t->pending) == NULL)
		return;
	if (t->notify != NULL)
		(void)clm_agent_notify(
		    clm_lua_plugin_agent(t->plugin), t->notify);
	else
		(void)clm_lua_plugin_callback(
		    t->plugin, lua_push_fn, &t->fn_ref);
	lua_timer_free(t, true);
}

static void
lua_timer_teardown(struct clm_lua_pending *pending)
{
	lua_timer_free((struct lua_timer *)pending, false);
}

static struct lua_timer *
lua_timer_start(lua_State *L, struct clm_lua_plugin *plugin, lua_Integer ms)
{
	struct clm_host *host = get_host(plugin);
	const char *err = NULL;
	struct lua_timer *t;

	if (host == NULL || host->timer_set == NULL)
		luaL_error(L, "this host has no timers");
	if (ms < 0)
		ms = 0;
	t = calloc(1, sizeof(*t));
	if (t == NULL)
		luaL_error(L, "out of memory");
	t->plugin = plugin;
	t->host = host;
	t->ud_ref = t->fn_ref = LUA_NOREF;
	if (clm_lua_pending_add(plugin, &t->pending, lua_timer_teardown) < 0) {
		err = "plugin is shutting down";
	} else if (host->timer_set(host->ctx, (uint64_t)ms, lua_timer_fire, t,
	               &t->timer) < 0) {
		(void)clm_lua_pending_remove(&t->pending);
		err = "timer_set failed";
	}
	if (err == NULL)
		return t;
	free(t);
	luaL_error(L, "%s", err);
	return NULL;
}

/* clm.after(ms, fn) -> handle with :cancel(). fn runs once, later. */
static int
lua_clm_after(lua_State *L)
{
	struct clm_lua_plugin *plugin = get_plugin(L);
	lua_Integer ms = luaL_checkinteger(L, 1);
	struct lua_timer_ud *ud;
	struct lua_timer *t;
	int fn_ref, ud_ref;

	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_settop(L, 2);
	ud = lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->t = NULL;
	luaL_setmetatable(L, CLM_LUA_TIMER_META);
	lua_pushvalue(L, 3);
	ud_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pushvalue(L, 2);
	fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	t = lua_timer_start(L, plugin, ms);
	t->ud_ref = ud_ref;
	t->fn_ref = fn_ref;
	t->ud = ud;
	ud->t = t;
	return 1;
}

static int
lua_timer_cancel(lua_State *L)
{
	struct lua_timer_ud *ud = luaL_checkudata(L, 1, CLM_LUA_TIMER_META);
	struct lua_timer *t = ud->t;

	if (t == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	(void)clm_lua_pending_remove(&t->pending);
	lua_timer_free(t, true);
	lua_pushboolean(L, 1);
	return 1;
}

/*
 * clm.notify(text): deliver text to the agent as a new message. It starts a
 * turn when the agent is idle, or joins the running one. Delivery happens
 * from the event loop, never inside the calling Lua code.
 */
static int
lua_clm_notify(lua_State *L)
{
	struct clm_lua_plugin *plugin = get_plugin(L);
	const char *text = luaL_checkstring(L, 1);
	struct lua_timer *t;
	char *copy;

	t = lua_timer_start(L, plugin, 0);
	copy = strdup(text);
	if (copy == NULL) {
		(void)clm_lua_pending_remove(&t->pending);
		lua_timer_free(t, true);
		return luaL_error(L, "out of memory");
	}
	t->notify = copy;
	return 0;
}

/* clm.getenv(name) -> string or nil */
static int
lua_clm_getenv(lua_State *L)
{
	const char *v = getenv(luaL_checkstring(L, 1));

	if (v == NULL)
		lua_pushnil(L);
	else
		lua_pushstring(L, v);
	return 1;
}

static const luaL_Reg proc_methods[] = {
    {"kill", lua_proc_kill},
    {"running", lua_proc_running},
    {NULL, NULL},
};

static const luaL_Reg timer_methods[] = {
    {"cancel", lua_timer_cancel},
    {NULL, NULL},
};

static void
lua_new_meta(lua_State *L, const char *name, const luaL_Reg *methods)
{
	luaL_newmetatable(L, name);
	lua_newtable(L);
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
}

void
clm_lua_proc_open(lua_State *L, struct clm_lua_plugin *plugin)
{
	(void)plugin;
	lua_new_meta(L, CLM_LUA_PROC_META, proc_methods);
	lua_new_meta(L, CLM_LUA_TIMER_META, timer_methods);
	lua_pushcfunction(L, lua_clm_spawn);
	lua_setfield(L, -2, "spawn");
	lua_pushcfunction(L, lua_clm_exec);
	lua_setfield(L, -2, "exec");
	lua_pushcfunction(L, lua_clm_after);
	lua_setfield(L, -2, "after");
	lua_pushcfunction(L, lua_clm_notify);
	lua_setfield(L, -2, "notify");
	lua_pushcfunction(L, lua_clm_getenv);
	lua_setfield(L, -2, "getenv");
}
