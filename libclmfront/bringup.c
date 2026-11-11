// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <string.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/host_uv.h"
#include "clm/lua_plugin.h"
#include "clm/mcp.h"
#include "bringup.h"
#include "plugin_setup.h"
#include "seed_plugins.h"
#include "xdg.h"
#include "banned.h"

static void
start_plugins(const struct clm_front_opts *o, struct clm_front_agent *out)
{
	autofree char *def = NULL;
	const char *dir = o->plugin_dir;

	if (clm_lua_env_new(out->agent, &out->lua_env) != 0) {
		out->lua_env = NULL;
		return;
	}
	if (o->lcfg != NULL)
		clm_lua_env_set_config_from(out->lua_env, o->lcfg);
	if (dir == NULL) {
		def = xdg_config_path("clm/plugins");
		if (def != NULL && o->seed_plugins)
			(void)clm_seed_default_plugins(def);
		dir = def;
	}
	clm_cli_load_plugins(out->lua_env, dir, o->lcfg, o->opt_plugins,
	    o->status, o->status_user);
	if (o->agent_plugins != NULL) {
		autofree char *adir = xdg_config_path("clm/agents");
		char path[512];

		if (adir != NULL) {
			(void)snprintf(path, sizeof(path), "%s/%s", adir,
			    o->agent_plugins);
			(void)clm_lua_load_plugins(out->lua_env, path);
		}
	}
}

int
clm_front_agent_start(
    const struct clm_front_opts *o, struct clm_front_agent *out)
{
	int r;

	memset(out, 0, sizeof(*out));
	r = clm_agent_new(o->cfg, o->host, o->callbacks, o->user, &out->agent);
	if (r < 0) {
		out->agent = NULL;
		return r;
	}
	if (o->spool != NULL)
		clm_agent_set_spool(out->agent, o->spool, o->spool_user);
	if (o->effort != NULL &&
	    clm_agent_set_effort(out->agent, o->effort) < 0 &&
	    o->status != NULL)
		o->status(
		    "could not set the reasoning effort", true, o->status_user);
	(void)clm_agent_set_compact_effort(out->agent, o->compact_effort);

	/* Peer messaging is optional: a bind failure (no runtime dir, a
	 * hostile /tmp) costs the socket, not the session. */
	if (clm_peer_start(out->agent, o->loop, o->peer_id,
	        o->peer_name != NULL ? o->peer_name : "clm", o->cfg->model,
	        o->peer_cb, o->peer_user, &out->peer) == 0)
		(void)clm_peer_register_tools(out->agent);
	else
		out->peer = NULL;

	(void)clm_tools_register_shell(out->agent);
	(void)clm_tools_register_bg(out->agent);
	(void)clm_tools_register_monitor(out->agent);
	start_plugins(o, out);
	return 0;
}

void
clm_front_agent_connect_mcp(
    struct clm_front_agent *a, const struct clm_front_opts *o)
{
	a->mcp_clients = clm_cli_connect_mcp_servers(a->agent, o->loop, o->lcfg,
	    o->status, o->status_user, &a->mcp_client_count);
}

void
clm_front_agent_stop(struct clm_front_agent *a)
{
	if (a == NULL)
		return;
	/* The plugins and MCP clients unregister their tools, which touches
	 * the agent, so they go first. The peer holds the agent too. */
	clm_lua_env_free(a->lua_env);
	clm_cli_free_mcp_servers(a->mcp_clients, a->mcp_client_count);
	clm_peer_free(a->peer);
	clm_agent_free(a->agent);
	memset(a, 0, sizeof(*a));
}
