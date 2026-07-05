// SPDX-License-Identifier: ISC
#ifndef CLM_MCP_H
#define CLM_MCP_H

/*
 * MCP (Model Context Protocol) client -- desktop uv/curl layer, like
 * tool_shell.c: it needs a real event loop and (for the stdio transport) a
 * subprocess, so it lives here rather than in the portable core. Talks to one
 * MCP server, runs the "initialize" handshake and "tools/list", then
 * registers each remote tool with clm_tool_add(). A registered tool's invoke
 * shim sends "tools/call" and completes the clm invocation when the MCP
 * response arrives.
 *
 * Two transports:
 *   - CLM_MCP_HTTP:  JSON-RPC over HTTP POST (the http_async engine already
 *     used for chat completions). Streamable-HTTP framing (SSE) is not yet
 *     handled; only a single JSON response body per request.
 *   - CLM_MCP_STDIO: JSON-RPC over the stdin/stdout of a spawned subprocess,
 *     newline-delimited (the framing most MCP stdio servers use).
 */

#include <stdint.h>

#include "clm/clm_export.h"

struct clm_agent;
struct uv_loop_s;

enum clm_mcp_transport {
	CLM_MCP_STDIO,
	CLM_MCP_HTTP,
};

/*
 * One server's connection config. All string fields are borrowed for the
 * duration of clm_mcp_connect (copied internally); they need not outlive it.
 */
struct clm_mcp_server_cfg {
	const char *name; /* short, unique; prefixed onto registered tool names */
	enum clm_mcp_transport transport;

	/* CLM_MCP_STDIO: argv[0] is the executable, NULL-terminated. */
	char *const *argv;

	/* CLM_MCP_HTTP: endpoint URL and optional bearer token. */
	const char *url;
	const char *api_key;

	uint64_t timeout_ms; /* per tool-call deadline; 0 => library default */
};

struct clm_mcp_client;

/*
 * Connect to an MCP server: start the transport, run "initialize" then
 * "tools/list", and register each discovered tool on `agent` (name is
 * "mcp__<server_cfg.name>__<tool name>", matching the scheme Claude Code uses
 * for MCP-sourced tools, to avoid collisions across servers).
 * Asynchronous: returns 0 once connection has started (negative errno if the
 * transport failed to even start, e.g. spawn failure), and later calls
 * on_ready(status, tool_count, user) -- status 0 on success, negative errno
 * (and tool_count 0) if the handshake or tools/list failed.
 *
 * *out is set immediately (even before on_ready fires) so the caller can hold
 * it for clm_mcp_client_free; the client stays alive until freed regardless of
 * on_ready's outcome.
 */
CLM_API int clm_mcp_connect(struct clm_agent *agent, struct uv_loop_s *loop,
    const struct clm_mcp_server_cfg *server_cfg,
    void (*on_ready)(int status, size_t tool_count, void *user), void *user,
    struct clm_mcp_client **out);

/*
 * Tear down the client: unregisters every tool it had registered on the
 * agent (so the model stops seeing them and no dangling reference is left
 * behind) and fails any in-flight call immediately -- both happen
 * synchronously, before this returns, so it is always safe to call
 * clm_agent_free() right after this, even with a call in flight or the
 * agent otherwise still in active use.
 *
 * Releasing the client's own memory (and, for stdio, actually killing and
 * reaping the subprocess) may be deferred to a subsequent iteration of
 * `loop` -- a live uv_process_t can only be closed from within its own exit
 * callback, so a still-running subprocess is killed here but its handles
 * are not released until the kernel reaps it. No action is required from
 * the caller beyond continuing to run the loop as normal (or letting the
 * process exit, if this is happening at final shutdown -- either reclaims
 * it). Safe to call with NULL.
 */
CLM_API void clm_mcp_client_free(struct clm_mcp_client *client);

#endif /* CLM_MCP_H */
