// SPDX-License-Identifier: ISC
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>

#include <json-c/json.h>

#include "clm/clm.h"
#include "clm/lua_plugin.h"
#include "clm/log.h"
#include "useful.h"
#include "banned.h"

/* Forward declarations for modules registered into each plugin state. */
int clm_lua_json_open(lua_State *L);
void clm_lua_push_json_value(lua_State *L, struct json_object *obj);
int clm_lua_http_open(lua_State *L, struct clm_agent *agent);

#define CLM_LUA_MEM_LIMIT (8 * 1024 * 1024) /* 8 MiB per plugin */
#define CLM_LUA_EXEC_TIMEOUT_MS 30000u     /* default: 30s CPU timeout */
#define CLM_LUA_HOOK_INTERVAL 10000        /* check deadline every N instructions */
#define CLM_LUA_HTTP_MAX_INFLIGHT 8        /* max concurrent HTTP requests */
#define CLM_LUA_HTTP_MAX_PER_CALL 32       /* max total HTTP requests per tool call */
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
};

/* Top-level environment holding all plugins. */
struct clm_lua_env {
	struct clm_agent *agent;
	struct clm_lua_plugin *plugins;
	size_t count;
	size_t cap;
	uint64_t exec_timeout_ms; /* global execution timeout */
	/* Global budget defaults. */
	size_t http_max_inflight;
	size_t http_max_per_call;
	size_t json_decode_max;
};

/* Budget helpers called from lua_http.c (defined below). */
void clm_lua_http_done(struct clm_lua_plugin *plugin);
int clm_lua_http_start(struct clm_lua_plugin *plugin);
void clm_lua_budget_report(struct clm_lua_plugin *plugin, const char *name);

/* ------------------------------------------------------------------ */
/* Capped allocator                                                    */
/* ------------------------------------------------------------------ */

static void *
lua_capped_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct clm_lua_plugin *p = ud;

	if (nsize == 0) {
		p->mem_used -= osize;
		free(ptr);
		return NULL;
	}
	if (p->mem_used - osize + nsize > p->mem_limit)
		return NULL; /* refused; Lua raises OOM */
	void *np = realloc(ptr, nsize);
	if (np == NULL)
		return NULL;
	p->mem_used = p->mem_used - osize + nsize;
	if (p->mem_used > p->budget.peak_mem)
		p->budget.peak_mem = p->mem_used;
	return np;
}

/* ------------------------------------------------------------------ */
/* Invocation context userdata                                         */
/* ------------------------------------------------------------------ */

#define CLM_LUA_CTX_META "clm_tool_ctx"

struct lua_tool_ctx {
	struct clm_tool_invocation *inv;
	lua_State *main_L;  /* plugin's main state */
	int co_ref;         /* registry ref keeping coroutine alive */
	bool completed;
};

static struct lua_tool_ctx *
check_ctx(lua_State *L, int idx)
{
	return luaL_checkudata(L, idx, CLM_LUA_CTX_META);
}

static int
lua_ctx_complete(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	const char *result;

	if (ctx->completed)
		return luaL_error(L, "tool already completed");
	result = luaL_checkstring(L, 2);
	ctx->completed = true;
	clm_tool_complete(ctx->inv, result);
	return 0;
}

static int
lua_ctx_fail(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	const char *msg;

	if (ctx->completed)
		return luaL_error(L, "tool already completed");
	msg = luaL_checkstring(L, 2);
	ctx->completed = true;
	clm_tool_fail(ctx->inv, msg);
	return 0;
}

static int
lua_ctx_args_raw(lua_State *L)
{
	struct lua_tool_ctx *ctx = check_ctx(L, 1);
	const char *args = clm_tool_invocation_args(ctx->inv);

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
	struct lua_tool_ctx *ctx;
	int nres, rc;

	/* Reset per-call budget counters. */
	plugin->budget.http_total = 0;
	plugin->budget.call_start_ns = clock_ns();

	/* Create a coroutine for this invocation. */
	co = lua_newthread(L);
	if (co == NULL) {
		clm_tool_fail(inv, "failed to create Lua coroutine");
		return;
	}
	/* Keep coroutine rooted on the main stack while it runs. We'll pop
	 * it once the tool completes (synchronously or after resume). */
	int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	/* Push the invoke function onto the coroutine stack. */
	lua_rawgeti(co, LUA_REGISTRYINDEX, tu->invoke_ref);

	/* Decode tool args directly in C (avoids unprotected lua_call and the
	 * C->Lua->C round trip through json.decode). */
	const char *args_str = clm_tool_invocation_args(inv);
	struct json_object *args_obj = json_tokener_parse(args_str ? args_str : "{}");
	if (args_obj == NULL) {
		clm_tool_fail(inv, "invalid tool arguments (malformed JSON)");
		luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
		return;
	}
	clm_lua_push_json_value(co, args_obj);
	json_object_put(args_obj);

	/* Push ctx userdata. */
	ctx = lua_newuserdatauv(co, sizeof(*ctx), 0);
	ctx->inv = inv;
	ctx->main_L = L;
	ctx->co_ref = co_ref;
	ctx->completed = false;
	luaL_setmetatable(co, CLM_LUA_CTX_META);

	/* Store co_ref on the coroutine's registry so http functions
	 * (which run on `co`) can retrieve it for the async callback. */
	lua_pushinteger(co, co_ref);
	lua_setfield(co, LUA_REGISTRYINDEX, "_clm_co_ref");

	/* Store tool name for budget attribution in async callbacks. */
	lua_pushstring(co, clm_tool_invocation_name(inv));
	lua_setfield(co, LUA_REGISTRYINDEX, "_clm_tool_name");

	/* Set execution deadline and install the count hook. The hook fires
	 * every N instructions and raises if the deadline has passed. */
	uint64_t timeout = clm_tool_invocation_timeout_ms(inv);
	if (timeout == 0)
		timeout = CLM_LUA_EXEC_TIMEOUT_MS;
	plugin->deadline_ns = clock_ns() + timeout * 1000000ULL;
	lua_sethook(co, lua_exec_hook, LUA_MASKCOUNT, CLM_LUA_HOOK_INTERVAL);

	/* Resume the coroutine with (args_table, ctx). */
	nres = 0;
	rc = lua_resume(co, L, 2, &nres);

	if (rc == LUA_OK) {
		/* Coroutine returned normally (synchronous tool). */
		plugin->deadline_ns = 0;
		lua_sethook(co, NULL, 0, 0);
		if (!ctx->completed)
			clm_tool_fail(inv, "plugin returned without calling ctx:complete/ctx:fail");
		clm_lua_budget_report(plugin, clm_tool_invocation_name(inv));
		luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
	} else if (rc == LUA_YIELD) {
		/* Coroutine yielded (waiting for async op like HTTP).
		 * Clear the hook while suspended — the plugin isn't running
		 * Lua bytecode during the wait. */
		plugin->deadline_ns = 0;
		lua_sethook(co, NULL, 0, 0);
	} else {
		/* Runtime error (includes timeout from the exec hook). */
		plugin->deadline_ns = 0;
		lua_sethook(co, NULL, 0, 0);
		const char *err = lua_tostring(co, -1);
		clm_debug("lua plugin error: %s", err ? err : "(unknown)");
		if (!ctx->completed)
			clm_tool_fail(inv, err ? err : "Lua runtime error");
		clm_lua_budget_report(plugin, clm_tool_invocation_name(inv));
		luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
	}
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
	lua_setglobal(L, "clm");

	/* Register ctx metatable. */
	register_ctx_meta(L);

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

	/* Grow the plugins array. */
	if (env->count >= env->cap) {
		size_t ncap = env->cap ? env->cap * 2 : 4;
		struct clm_lua_plugin *np;
		np = realloc(env->plugins, ncap * sizeof(*np));
		if (np == NULL)
			return -ENOMEM;
		env->plugins = np;
		env->cap = ncap;
	}

	plugin = &env->plugins[env->count];
	memset(plugin, 0, sizeof(*plugin));
	plugin->agent = env->agent;
	plugin->mem_limit = CLM_LUA_MEM_LIMIT;
	plugin->budget.http_max_inflight = env->http_max_inflight;
	plugin->budget.http_max_per_call = env->http_max_per_call;
	plugin->budget.json_decode_max = env->json_decode_max;
	plugin->path = strdup(path);
	if (plugin->path == NULL)
		return -ENOMEM;

	/* Create a state with a capped allocator. */
	L = lua_newstate(lua_capped_alloc, plugin);
	if (L == NULL) {
		free(plugin->path);
		return -ENOMEM;
	}
	plugin->L = L;

	/* Set up the sandbox. */
	sandbox_state(L, plugin);

	/* Execute the plugin file. */
	if (luaL_loadfile(L, path) != LUA_OK) {
		const char *err = lua_tostring(L, -1);
		clm_debug("lua: failed to load %s: %s", path, err ? err : "?");
		lua_close(L);
		free(plugin->path);
		return -EINVAL;
	}
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		const char *err = lua_tostring(L, -1);
		clm_debug("lua: error executing %s: %s", path, err ? err : "?");
		lua_close(L);
		free(plugin->path);
		return -EINVAL;
	}

	env->count++;
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
	env->exec_timeout_ms = CLM_LUA_EXEC_TIMEOUT_MS;
	env->http_max_inflight = CLM_LUA_HTTP_MAX_INFLIGHT;
	env->http_max_per_call = CLM_LUA_HTTP_MAX_PER_CALL;
	env->json_decode_max = CLM_LUA_JSON_DECODE_MAX;
	*out = env;
	return 0;
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

	size_t dirlen = strlen(dir);
	while ((ent = readdir(d)) != NULL) {
		size_t nlen = strlen(ent->d_name);
		char *path;

		if (nlen < 5)
			continue;
		if (strcmp(ent->d_name + nlen - 4, ".lua") != 0)
			continue;

		/* dir + '/' + name + '\0' */
		path = malloc(dirlen + 1 + nlen + 1);
		if (path == NULL)
			continue;
		(void)snprintf(path, dirlen + 1 + nlen + 1, "%s/%s", dir, ent->d_name);

		int r = load_one_plugin(env, path);
		if (r < 0)
			clm_debug("lua: skipping %s (error %d)", path, r);
		else
			loaded++;
		free(path);
	}
	closedir(d);

	clm_debug("lua: loaded %d plugin(s) from %s", loaded, dir);
	return 0;
}

CLM_API void
clm_lua_env_free(struct clm_lua_env *env)
{
	if (env == NULL)
		return;
	for (size_t i = 0; i < env->count; i++) {
		struct clm_lua_plugin *p = &env->plugins[i];
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
	}
	free(env->plugins);
	free(env);
}
