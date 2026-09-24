// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clm/lua_plugin.h"
#include "plugin_setup.h"
#include "banned.h"

static void
load_opt(struct clm_lua_env *env, const char *dir, const char *name,
    clm_cli_plugin_status_cb status_cb, void *status_user)
{
	char msg[512];
	int r = clm_lua_load_plugin(env, dir, name);

	if (r == 0 || status_cb == NULL)
		return;
	if (name[0] == '\0' || name[0] == '.' || strchr(name, '/') != NULL)
		(void)snprintf(
		    msg, sizeof(msg), "plugin %s: not a plugin name", name);
	else if (r == -ENOENT)
		(void)snprintf(msg, sizeof(msg), "plugin %s: no %s/opt/%s.lua",
		    name, dir, name);
	else if (r == -EINVAL)
		(void)snprintf(msg, sizeof(msg),
		    "plugin %s: failed to load (set CLM_DEBUG_LOG for the "
		    "error)",
		    name);
	else
		(void)snprintf(
		    msg, sizeof(msg), "plugin %s: %s", name, strerror(-r));
	status_cb(msg, status_user);
}

static bool
listed(char **list, const char *name)
{
	for (size_t i = 0; list != NULL && list[i] != NULL; i++)
		if (strcmp(list[i], name) == 0)
			return true;
	return false;
}

void
clm_cli_load_plugins(struct clm_lua_env *env, const char *dir,
    struct clm_lua_cfg *lcfg, const char *const *extra,
    clm_cli_plugin_status_cb status_cb, void *status_user)
{
	char **names = NULL;

	if (env == NULL || dir == NULL)
		return;
	(void)clm_lua_load_plugins(env, dir);
	if (lcfg != NULL)
		names = clm_lua_cfg_get_str_list(lcfg, "plugins");
	for (size_t i = 0; names != NULL && names[i] != NULL; i++)
		load_opt(env, dir, names[i], status_cb, status_user);
	for (size_t i = 0; extra != NULL && extra[i] != NULL; i++) {
		/* Named twice is still one plugin. */
		bool dup = listed(names, extra[i]);

		for (size_t j = 0; j < i && !dup; j++)
			dup = strcmp(extra[j], extra[i]) == 0;
		if (!dup)
			load_opt(env, dir, extra[i], status_cb, status_user);
	}
	clm_lua_cfg_free_str_list(names);
}
