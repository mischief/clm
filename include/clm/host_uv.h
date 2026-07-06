// SPDX-License-Identifier: ISC
#ifndef CLM_HOST_UV_H
#define CLM_HOST_UV_H

/*
 * Desktop host adapter: a struct clm_host backed by libcurl (transport) and
 * libuv (event loop + timers). This is the glue that keeps the agent core free
 * of uv/curl; the CLI/TUI creates a loop, wraps it here, and hands the host to
 * clm_agent_new.
 */
#include <uv.h>

#include "clm/host.h"
#include "clm/clm_export.h"

/*
 * Create a clm_host driving the given loop. The host BORROWS the loop (it
 * registers handles but never runs or closes it). Returns 0 and stores the
 * host in *out, or a negative errno. Free with clm_host_uv_free.
 */
CLM_API int clm_host_uv_new(uv_loop_t *loop, struct clm_host **out);

CLM_API void clm_host_uv_free(struct clm_host *host);

/*
 * Register the shell_exec builtin (runs a command via uv_spawn). It lives in
 * this uv layer, not the portable core, so a caller wanting shell calls this in
 * addition to clm_tools_register_builtins(). Returns 0 or a negative errno.
 */
struct clm_agent;
CLM_API int clm_tools_register_shell(struct clm_agent *agent);

/*
 * Register the bg_exec builtin (starts a command via uv_spawn, same as
 * shell_exec, but returns immediately -- the result arrives later via
 * clm_agent_notify() as a fresh turn, not as this call's own result; see
 * lib/tool_bg.c for why). Independent of clm_tools_register_shell(); a
 * caller wanting both registers both. Returns 0 or a negative errno.
 */
CLM_API int clm_tools_register_bg(struct clm_agent *agent);

#endif /* CLM_HOST_UV_H */
