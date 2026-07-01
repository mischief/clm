// SPDX-License-Identifier: ISC
#ifndef CLM_LUA_PLUGIN_H
#define CLM_LUA_PLUGIN_H

#include "clm/clm_export.h"

struct clm_agent;
struct clm_lua_env;

/*
 * Create a Lua plugin environment bound to the given agent.
 * Returns 0 on success, negative errno on failure.
 */
CLM_API int clm_lua_env_new(struct clm_agent *agent, struct clm_lua_env **out);

/*
 * Set per-tool configuration. tool_config_json is a JSON object string
 * keyed by tool/plugin name, e.g. {"web_search":{"api_key":"..."}}.
 * Each plugin receives only its own subtable as clm.config.
 * Must be called before clm_lua_load_plugins. Ownership of the string
 * is NOT transferred (it is copied internally).
 */
CLM_API int clm_lua_env_set_config(struct clm_lua_env *env,
    const char *tool_config_json);

/*
 * Load all .lua files from the given directory. Each file gets its own
 * sandboxed lua_State with a capped allocator. The plugin is executed
 * immediately; it is expected to call clm.tool_register() to register tools.
 * Returns 0 on success (even if no .lua files found), negative errno on
 * hard failure (e.g. directory unreadable).
 */
CLM_API int clm_lua_load_plugins(struct clm_lua_env *env, const char *dir);

/*
 * Free all Lua states and the environment. Safe to call with NULL.
 */
CLM_API void clm_lua_env_free(struct clm_lua_env *env);

/*
 * Load a Lua config file and extract the "tools" table as a JSON string.
 * The caller owns the returned string (free with free()). Returns NULL
 * on failure (file not found, parse error, no tools table).
 * This is a standalone utility — does not require a clm_lua_env.
 */
CLM_API char *clm_lua_load_config(const char *path);

#endif /* CLM_LUA_PLUGIN_H */
