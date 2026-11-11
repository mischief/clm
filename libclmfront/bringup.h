// SPDX-License-Identifier: ISC
/*
 * Starting an agent the way every frontend wants it: the agent itself, its
 * spool and effort, the peer socket and tools, the shell tools, and the lua
 * plugins. The tui, the headless cli and clm-gtk each did this by hand, and
 * the copies had drifted: after a /agent switch the tui's new agent had lost
 * its peer tools, and the old peer socket pointed at a freed agent.
 *
 * MCP servers are a second step, clm_front_agent_connect_mcp(), because the
 * headless cli checks for a pre_tool hook between the two and must not start
 * server processes just to exit.
 */
#ifndef CLM_FRONT_BRINGUP_H
#define CLM_FRONT_BRINGUP_H

#include <stdbool.h>
#include <stddef.h>

#include "clm/clm.h"
#include "clm/peer.h"
#include "mcp_setup.h"

struct clm_host;
struct clm_lua_cfg;
struct clm_lua_env;
struct clm_mcp_client;
struct uv_loop_s;

/* How long a session log outlives its last write, unless config says
 * otherwise (session_keep_days = 0 keeps everything). */
#define CLM_SESSION_KEEP_DAYS 90

struct clm_front_opts {
	const struct clm_cfg *cfg;
	struct clm_host *host;
	struct uv_loop_s *loop;
	const struct clm_callbacks *callbacks;
	void *user;
	struct clm_lua_cfg *lcfg; /* NULL: no config.lua */

	/* Reasoning effort for the connection; NULL takes the backend's. */
	const char *effort;
	const char *compact_effort;

	/* Where a cut tool result goes whole; NULL keeps only the sizes. */
	clm_spool_fn spool;
	void *spool_user;

	/*
	 * The peer socket. peer_id is the session id to announce on; NULL
	 * makes this agent client only, able to list and message the others
	 * without being found. peer_cb hears what another agent says.
	 */
	const char *peer_id;
	const char *peer_name; /* NULL: "clm" */
	clm_peer_msg_cb peer_cb;
	void *peer_user;

	/* Plugins: plugin_dir, or the XDG default when NULL. */
	const char *plugin_dir;
	bool seed_plugins; /* write the builtin set into the default dir */
	const char *const *opt_plugins;
	const char *agent_plugins; /* also load agents/<name>/, or NULL */

	/* Plugin and MCP progress and failures; NULL prints to stderr. */
	clm_cli_mcp_status_cb status;
	void *status_user;
};

struct clm_front_agent {
	struct clm_agent *agent;
	struct clm_lua_env *lua_env; /* NULL when the plugins did not start */
	struct clm_peer *peer;       /* NULL without a peer socket */
	struct clm_mcp_client **mcp_clients;
	size_t mcp_client_count;
};

/* Returns 0, or what clm_agent_new returns. Everything after it is best
 * effort: a failure costs that feature, not the agent. */
int clm_front_agent_start(
    const struct clm_front_opts *o, struct clm_front_agent *out);

/* Connect the MCP servers config.lua names. The agent must have been
 * started with the same o. */
void clm_front_agent_connect_mcp(
    struct clm_front_agent *a, const struct clm_front_opts *o);

/*
 * Move the peer socket onto a new session id. Other agents address this one
 * by that id, so a new session needs a new socket. A no-op when the agent
 * has no peer. Returns 0, -ENOENT with nothing to move, or what
 * clm_peer_start returns.
 */
int clm_front_agent_rebind_peer(struct clm_front_agent *a,
    struct uv_loop_s *loop, const char *session_id, const char *name,
    const char *model, clm_peer_msg_cb cb, void *user);

/*
 * Give the agent the scratch directory of session_id, and the prompt block
 * that names it. The block is malloc'd and the caller's to keep and to free
 * when the next one replaces it; NULL if the directory could not be made.
 */
char *clm_front_scratch_rebind(struct clm_agent *agent, const char *session_id);

/* Remove session logs and scratch directories past session_keep_days of
 * config.lua (the default, when lcfg is NULL or silent). Best effort. */
void clm_front_sweep_old(struct clm_lua_cfg *lcfg);

/*
 * Take apart "[message from agent ID (NAME)]\nTEXT\n(hint)", the frame a
 * peer message wears in the agent's history. Returns one malloc'd buffer
 * that from, name and text point into, or NULL when content is not such a
 * frame. The reply hint is dropped from text.
 */
char *clm_front_peer_unframe(const char *content, const char **from,
    const char **name, const char **text);

/* Free the plugins, the MCP clients, the peer and the agent, in the order
 * that is safe; the host and the loop are the caller's. Safe on a zeroed
 * struct, and leaves it zeroed. */
void clm_front_agent_stop(struct clm_front_agent *a);

#endif /* CLM_FRONT_BRINGUP_H */
