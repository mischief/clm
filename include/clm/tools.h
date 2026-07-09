// SPDX-License-Identifier: ISC
#ifndef CLM_TOOLS_H
#define CLM_TOOLS_H

#include <stddef.h>
#include <sys/queue.h>

#include <cjson/cJSON.h>

#include "clm/clm.h"
#include "clm/history.h"

struct clm_agent;

/*
 * A registered tool: an owned copy of the clm_tool_def the caller provided.
 * Held on a TAILQ (see struct clm_tool_list below) rather than an array so
 * that clm_tool_remove() never has to move or invalidate another tool's
 * node -- important because struct clm_tool_invocation.def is a raw pointer
 * into one of these nodes, held for the life of an invocation (including
 * across an async permission prompt, which clm_tool_permission_respond may
 * answer much later; see clm_tool_permission_respond in clm.h).
 *
 * A tool being removed while it still has an incomplete invocation is
 * handled by "removed"/"inflight": clm_tool_remove unlinks the node
 * immediately (so it stops being dispatched to or advertised) but only
 * frees it once inflight drops to zero (see inv_finalize/find_tool in
 * tools.c).
 */
struct clm_tool {
	TAILQ_ENTRY(clm_tool) entries;

	char *name;
	char *description;   /* owned, may be NULL */
	char *params_schema; /* owned JSON string, may be NULL */
	clm_tool_fn invoke;
	void *user;
	size_t output_cap;
	uint64_t timeout_ms;
	unsigned flags;

	/* Session memory of an _ALWAYS permission decision for this tool. */
	bool remembered;      /* a decision has been made */
	bool remember_allow;  /* true=always allow, false=always deny */

	/* Removal bookkeeping; see the struct comment above. */
	bool removed;      /* clm_tool_remove was called; unlinked, zombie */
	unsigned inflight; /* invocations with def == this node, not yet finalized */
};
TAILQ_HEAD(clm_tool_list, clm_tool);

/* Register the three built-in tools (read_file, write_file, shell_exec). */
int clm_tools_register_builtins(struct clm_agent *agent);

/* Free every node on a tool registry list (name/description/params_schema
 * strings, then the node itself). Used at agent teardown only -- by then no
 * invocation can be in flight, so removed-but-zombie nodes are freed too. */
void clm_tools_free_registry(struct clm_tool_list *tools);

/*
 * Build the OpenAI "tools" schema array for the agent's registered tools,
 * injecting the timeout_ms / output_cap parameters for tools that opted in.
 * Caller owns the returned object (cJSON_Delete), or NULL on failure.
 */
cJSON *clm_tools_build_schema(const struct clm_agent *agent);

/*
 * Dispatch a batch of assistant tool calls. Records the assistant tool-call
 * message and one tool-result message per call into history, firing
 * on_tool_begin/on_tool_result. Runs asynchronously: returns 0 once the batch
 * is started (negative errno on setup failure), and calls clm_agent_tools_done
 * when every call has completed. tool_calls is the borrowed cJSON array from
 * the model response.
 */
int clm_tools_dispatch(struct clm_agent *agent, cJSON *tool_calls);

/* Abort an in-flight batch (best effort), used during agent teardown. */
void clm_tools_cancel(struct clm_agent *agent);

/*
 * Sever the active batch (if any) from an agent that is about to be freed.
 * Requests cancellation the same way clm_tools_cancel() does, but also
 * retires every invocation that cannot possibly still be running (parked on
 * a permission prompt or a rate-limit timer) and marks the batch itself as
 * detached. A detached batch survives clm_agent_free(): any invocation still
 * genuinely in flight (e.g. a shell/bg child process) completes later,
 * asynchronously, and its completion callback (clm_tool_complete/_fail) sees
 * the detached flag and retires quietly instead of touching the freed agent.
 * Call this from clm_agent_free() in place of clm_tools_cancel(); it leaves
 * agent->active_batch cleared.
 */
void clm_tools_detach(struct clm_agent *agent);

#endif /* CLM_TOOLS_H */
