// SPDX-License-Identifier: ISC
#ifndef CLM_TOOLS_H
#define CLM_TOOLS_H

#include <stddef.h>

#include <json-c/json.h>

#include "clm/clm.h"
#include "clm/history.h"

struct clm_agent;

struct clm_tool {
	enum clm_tool_type type;
	char *name;
};

/*
 * Build the OpenAI "tools" schema array for the agent's registered tools.
 * Caller owns the returned object (json_object_put), or NULL on failure.
 */
struct json_object *clm_tools_build_schema(const struct clm_agent *agent);

/*
 * Execute the tool named by call->name with call->args (raw JSON string).
 * On success *result is a heap-allocated clm_tool_result (caller frees with
 * clm_tool_result_free). Returns 0 on success, negative errno on failure.
 * Tool-level failures (missing file, bad args) are reported as a successful
 * result whose content is an error message, so the model can recover.
 */
int clm_tool_execute(struct clm_agent *agent, const struct clm_tool_call *call, struct clm_tool_result **result);

#endif /* CLM_TOOLS_H */
