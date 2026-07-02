// SPDX-License-Identifier: ISC
#ifndef CLM_LUA_PLUGIN_H
#define CLM_LUA_PLUGIN_H

#include "clm/clm_export.h"

struct clm_agent;
struct clm_lua_env;
struct clm_lua_cfg;

/*
 * Create a Lua plugin environment bound to the given agent.
 * Returns 0 on success, negative errno on failure.
 */
CLM_API int clm_lua_env_new(struct clm_agent *agent, struct clm_lua_env **out);

/*
 * Set per-tool configuration from a clm_lua_cfg. Extracts the "tools"
 * subtable and makes each plugin's section available as clm.config.
 * Must be called before clm_lua_load_plugins.
 */
CLM_API int clm_lua_env_set_config_from(struct clm_lua_env *env,
    struct clm_lua_cfg *cfg);

/*
 * Legacy: set per-tool config from a JSON string.
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

/* ------------------------------------------------------------------ */
/* Config state: a long-lived lua_State holding the parsed config.lua  */
/* ------------------------------------------------------------------ */

/*
 * Load and evaluate a config.lua file. Keeps the lua_State alive for
 * subsequent queries. Returns NULL on failure (missing file, parse error).
 */
CLM_API struct clm_lua_cfg *clm_lua_cfg_load(const char *path);

/*
 * Load an agent profile. If agent_name is non-NULL, loads
 * <agents_dir>/<agent_name>.lua and merges it. If NULL, reads the
 * "agent" key from the config to determine the name.
 * Returns 0 on success, -1 if the agent file is not found.
 */
CLM_API int clm_lua_cfg_load_agent(struct clm_lua_cfg *cfg,
    const char *agents_dir, const char *agent_name);

/*
 * Query a string field from the resolved agent config.
 * Returns NULL if the field is absent or not a string.
 * The returned pointer is valid for the lifetime of the cfg.
 */
CLM_API const char *clm_lua_cfg_get_str(struct clm_lua_cfg *cfg,
    const char *key);

/*
 * Query a string field from a named provider in config.providers.
 * e.g. clm_lua_cfg_provider_str(cfg, "huggingface", "url")
 * Returns NULL if not found.
 */
CLM_API const char *clm_lua_cfg_provider_str(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *key);

/*
 * Get the tools config as a JSON string (for clm_lua_env_set_config).
 * Caller owns the returned string (free with free()). NULL if no tools.
 */
CLM_API char *clm_lua_cfg_tools_json(struct clm_lua_cfg *cfg);

/*
 * Free the config state.
 */
CLM_API void clm_lua_cfg_free(struct clm_lua_cfg *cfg);

/* Legacy: load config and return tools JSON. Caller frees. */
CLM_API char *clm_lua_load_config(const char *path);

#endif /* CLM_LUA_PLUGIN_H */
