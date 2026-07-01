// SPDX-License-Identifier: ISC
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

/* Forward declaration. */
struct lua_tool_user;

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
};

/* Top-level environment holding all plugins. */
struct clm_lua_env {
	struct clm_agent *agent;
	struct clm_lua_plugin *plugins;
	size_t count;
	size_t cap;
};

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

	/* Resume the coroutine with (args_table, ctx). */
	nres = 0;
	rc = lua_resume(co, L, 2, &nres);

	if (rc == LUA_OK) {
		/* Coroutine returned normally (synchronous tool). */
		if (!ctx->completed)
			clm_tool_fail(inv, "plugin returned without calling ctx:complete/ctx:fail");
		luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
	} else if (rc == LUA_YIELD) {
		/* Coroutine yielded (waiting for async op like HTTP).
		 * The co_ref in ctx keeps the coroutine alive; the HTTP
		 * completion callback will retrieve it from ctx to unref. */
	} else {
		/* Runtime error. */
		const char *err = lua_tostring(co, -1);
		clm_debug("lua plugin error: %s", err ? err : "(unknown)");
		if (!ctx->completed)
			clm_tool_fail(inv, err ? err : "Lua runtime error");
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

	/* Register json module. */
	clm_lua_json_open(L);

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
