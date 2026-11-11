// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <string.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/host_uv.h"
#include "clm/lua_plugin.h"
#include "clm/mcp.h"
#include "clm/session.h"
#include "bringup.h"
#include "plugin_setup.h"
#include "seed_plugins.h"
#include "sysinfo.h"
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

int
clm_front_agent_rebind_peer(struct clm_front_agent *a, struct uv_loop_s *loop,
    const char *session_id, const char *name, const char *model,
    clm_peer_msg_cb cb, void *user)
{
	if (a->peer == NULL || session_id == NULL)
		return -ENOENT;
	clm_peer_free(a->peer);
	a->peer = NULL;
	/* The tools stay registered from the start; only the socket moves. */
	return clm_peer_start(a->agent, loop, session_id,
	    name != NULL ? name : "clm", model, cb, user, &a->peer);
}

char *
clm_front_scratch_rebind(struct clm_agent *agent, const char *session_id)
{
	autofree char *sysinfo = clm_cli_sysinfo();
	autofree char *dir = NULL;
	char *block;

	if (session_id == NULL)
		return NULL;
	block = clm_cli_scratch_attach(session_id, sysinfo, true, &dir);
	if (block != NULL)
		(void)clm_agent_set_system_prompt_suffix(agent, block);
	return block;
}

void
clm_front_sweep_old(struct clm_lua_cfg *lcfg)
{
	struct clm_session_info *infos = NULL;
	const char **live = NULL;
	size_t n = 0, i;
	int64_t keep_days = lcfg != NULL
	    ? clm_lua_cfg_get_int(
	          lcfg, "session_keep_days", CLM_SESSION_KEEP_DAYS)
	    : CLM_SESSION_KEEP_DAYS;

	if (keep_days > 0)
		(void)clm_session_gc(NULL, (unsigned)keep_days, NULL);
	if (clm_session_list(NULL, &infos, &n) != 0)
		return;
	if (n > 0) {
		live = calloc(n, sizeof(*live));
		if (live == NULL) {
			clm_session_list_free(infos, n);
			return;
		}
		for (i = 0; i < n; i++)
			live[i] = infos[i].id;
	}
	(void)clm_cli_scratch_gc(
	    live, n, (unsigned)(keep_days > 0 ? keep_days : 0));
	free(live);
	clm_session_list_free(infos, n);
}

char *
clm_front_peer_unframe(const char *content, const char **from,
    const char **name, const char **text)
{
	static const char pre[] = "[message from agent ";
	char *copy, *head, *nm, *body, *tail;

	if (content == NULL || strncmp(content, pre, sizeof(pre) - 1) != 0)
		return NULL;
	copy = strdup(content + sizeof(pre) - 1);
	if (copy == NULL)
		return NULL;
	body = strstr(copy, "]\n");
	if (body == NULL)
		goto bad;
	*body = '\0';
	body += 2;
	head = copy;
	nm = strstr(head, " (");
	if (nm == NULL || nm[strlen(nm) - 1] != ')')
		goto bad;
	*nm = '\0';
	nm += 2;
	nm[strlen(nm) - 1] = '\0';
	/* Drop the reply hint that deliver appends for the model. */
	tail = strrchr(body, '\n');
	if (tail != NULL && tail[1] == '(')
		*tail = '\0';
	*from = head;
	*name = nm;
	*text = body;
	return copy;
bad:
	free(copy);
	return NULL;
}
