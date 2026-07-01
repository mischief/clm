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

#endif /* CLM_LUA_PLUGIN_H */
