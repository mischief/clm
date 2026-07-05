// SPDX-License-Identifier: ISC
/* Shared config -> clm_mcp_connect glue for the clm binary's frontends
 * (plain REPL in main.c and the ncurses UI in tui.c). Not installed. */
#ifndef CLM_CLI_MCP_SETUP_H
#define CLM_CLI_MCP_SETUP_H

#include <uv.h>

struct clm_agent;
struct clm_lua_cfg;

/*
 * Read config.mcp_servers from lcfg (may be NULL, e.g. CLM_LUA disabled or no
 * config file found) and start a clm_mcp_connect() for each entry, on loop.
 * Connections are asynchronous and fire-and-forget: failures are printed to
 * stderr but never abort startup, since a dead MCP server shouldn't block
 * using clm for anything else.
 *
 * Returns a malloc'd array of *out_count client handles (NULL and *out_count
 * == 0 if none configured, lcfg is NULL, or every connect attempt failed to
 * even start). A NULL entry within the array marks a server whose connect
 * call failed synchronously (already reported to stderr) -- skip it when
 * freeing. The caller owns the array and every non-NULL entry in it: call
 * clm_mcp_client_free() on each, then free() the array itself, at teardown
 * (see clm_cli_free_mcp_servers below for a one-line helper that does this).
 */
struct clm_mcp_client **clm_cli_connect_mcp_servers(struct clm_agent *agent,
    uv_loop_t *loop, struct clm_lua_cfg *lcfg, size_t *out_count);

/*
 * Tear down every client returned by clm_cli_connect_mcp_servers: frees each
 * non-NULL entry (clm_mcp_client_free -- closes the transport, kills a stdio
 * server's subprocess) and the array itself. Safe to call with clients ==
 * NULL or count == 0.
 */
void clm_cli_free_mcp_servers(struct clm_mcp_client **clients, size_t count);

#endif /* CLM_CLI_MCP_SETUP_H */
