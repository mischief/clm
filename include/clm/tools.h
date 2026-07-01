// SPDX-License-Identifier: ISC
#ifndef CLM_TOOLS_H
#define CLM_TOOLS_H

#include <stddef.h>

#include <json-c/json.h>

#include "clm/clm.h"
#include "clm/history.h"

struct clm_agent;

/* A registered tool: an owned copy of the clm_tool_def the caller provided. */
struct clm_tool {
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
};

/* Register the three built-in tools (read_file, write_file, shell_exec). */
int clm_tools_register_builtins(struct clm_agent *agent);

/* Free a tool registry array (name/description/params_schema strings). */
void clm_tools_free_registry(struct clm_tool *tools, size_t count);

/*
 * Build the OpenAI "tools" schema array for the agent's registered tools,
 * injecting the timeout_ms / output_cap parameters for tools that opted in.
 * Caller owns the returned object (json_object_put), or NULL on failure.
 */
struct json_object *clm_tools_build_schema(const struct clm_agent *agent);

/*
 * Dispatch a batch of assistant tool calls. Records the assistant tool-call
 * message and one tool-result message per call into history, firing
 * on_tool_begin/on_tool_result. Runs asynchronously: returns 0 once the batch
 * is started (negative errno on setup failure), and calls clm_agent_tools_done
 * when every call has completed. tool_calls is the borrowed JSON array from
 * the model response.
 */
int clm_tools_dispatch(struct clm_agent *agent, struct json_object *tool_calls);

/* Abort an in-flight batch (best effort), used during agent teardown. */
void clm_tools_cancel(struct clm_agent *agent);

#endif /* CLM_TOOLS_H */
