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
 *
 * Before evaluating config.lua, also loads a sibling secrets.lua (same
 * directory) if present and exposes it as the global clm.secrets table,
 * so config.lua -- and, since clm_lua_cfg_load_agent reuses this same
 * lua_State, agent profile files too -- can write e.g.
 * api_key = clm.secrets.tavily instead of a literal key. A missing or
 * invalid secrets.lua yields an empty clm.secrets rather than failing.
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
 * Query an array-of-strings field, checking the resolved agent config
 * first and falling back to the top level (same precedence as
 * clm_lua_cfg_get_str). Returns a malloc'd NULL-terminated array of
 * malloc'd strings -- free with clm_lua_cfg_free_str_list() -- so the
 * result outlives the cfg, unlike the borrowed get_str pointers.
 * Non-string entries are skipped. NULL if the field is absent, not a
 * table, or holds no strings.
 */
CLM_API char **clm_lua_cfg_get_str_list(struct clm_lua_cfg *cfg,
    const char *key);

/* Free a list from clm_lua_cfg_get_str_list. Safe to call with NULL. */
CLM_API void clm_lua_cfg_free_str_list(char **list);

/*
 * Returns the agent name actually resolved by the most recent successful
 * clm_lua_cfg_load_agent() call, or NULL if that has not yet succeeded.
 * Use this (not config.agent via clm_lua_cfg_get_str) to display which
 * agent is actually active -- it reflects any -a/--agent override, not
 * just config.lua's static default.
 */
CLM_API const char *clm_lua_cfg_get_agent_name(struct clm_lua_cfg *cfg);

/*
 * Query a string field from a named provider in config.providers -- a
 * connection: url, api_key, kind (wire dialect), rate_tokens_per_sec,
 * rate_burst. e.g. clm_lua_cfg_provider_str(cfg, "huggingface", "url")
 * Returns NULL if not found.
 */
CLM_API const char *clm_lua_cfg_provider_str(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *key);

/* Same as above but for integer values. Returns fallback if not set. */
CLM_API int64_t clm_lua_cfg_provider_int(struct clm_lua_cfg *cfg,
    const char *provider_name, const char *key, int64_t fallback);

/*
 * Query a string field from a named model in config.models -- which
 * provider backs it and what to call it on the wire, plus per-model
 * overrides: provider, model, context_size, autocompact_pct.
 * e.g. clm_lua_cfg_model_str(cfg, "gpt-fast", "provider")
 * Returns NULL if not found.
 */
CLM_API const char *clm_lua_cfg_model_str(struct clm_lua_cfg *cfg,
    const char *model_name, const char *key);

/* Same as above but for integer values. Returns fallback if not set. */
CLM_API int64_t clm_lua_cfg_model_int(struct clm_lua_cfg *cfg,
    const char *model_name, const char *key, int64_t fallback);

/*
 * List the entry names (keys) of a top-level config table -- "providers"
 * or "models" -- for discoverability (e.g. a TUI /model or /provider
 * command with no argument listing what's available). Names are returned
 * in the table's natural iteration order (unspecified by Lua); the caller
 * should sort if a stable display order matters.
 *
 * Returns a malloc'd NULL-terminated array, free with
 * clm_lua_cfg_free_str_list(). Returns NULL if the table doesn't exist,
 * isn't a table, or has no string-keyed entries.
 */
CLM_API char **clm_lua_cfg_list_names(struct clm_lua_cfg *cfg,
    const char *table);

/*
 * Get the tools config as a JSON string (for clm_lua_env_set_config).
 * Caller owns the returned string (free with free()). NULL if no tools.
 */
CLM_API char *clm_lua_cfg_tools_json(struct clm_lua_cfg *cfg);

/*
 * Get the mcp_servers config as a JSON string (array of server tables; see
 * clm/mcp.h for the fields each entry needs per transport).
 * Caller owns the returned string (free with free()). NULL if unset.
 */
CLM_API char *clm_lua_cfg_mcp_servers_json(struct clm_lua_cfg *cfg);

/*
 * Free the config state.
 */
CLM_API void clm_lua_cfg_free(struct clm_lua_cfg *cfg);

/* Legacy: load config and return tools JSON. Caller frees. */
CLM_API char *clm_lua_load_config(const char *path);

#endif /* CLM_LUA_PLUGIN_H */
