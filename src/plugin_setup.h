// SPDX-License-Identifier: ISC
/* Plugin loading shared by the clm binary's frontends. Not installed. */
#ifndef CLM_CLI_PLUGIN_SETUP_H
#define CLM_CLI_PLUGIN_SETUP_H

struct clm_lua_cfg;
struct clm_lua_env;

typedef void (*clm_cli_plugin_status_cb)(const char *msg, void *user);

/*
 * Load every plugin in dir, then each opt-in plugin in dir/opt named by the
 * "plugins" list of lcfg (may be NULL) or by extra (NULL-terminated, may be
 * NULL). A missing or broken opt-in plugin is reported through status_cb.
 */
void clm_cli_load_plugins(struct clm_lua_env *env, const char *dir,
    struct clm_lua_cfg *lcfg, const char *const *extra,
    clm_cli_plugin_status_cb status_cb, void *status_user);

#endif /* CLM_CLI_PLUGIN_SETUP_H */
