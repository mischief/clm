// SPDX-License-Identifier: ISC
#ifndef CLM_H
#define CLM_H

#include <stddef.h>
#include <stdint.h>

#include "clm/clm_export.h"

struct clm_agent;
struct clm_cfg;

/* LLM provider types. */
enum clm_provider {
	CLM_PROVIDER_OPENAI,
	CLM_PROVIDER_OLLAMA,
	CLM_PROVIDER_ANTHROPIC,
};

/* Tool types. */
enum clm_tool_type {
	CLM_TOOL_FILE_READ,
	CLM_TOOL_FILE_WRITE,
	CLM_TOOL_SHELL_EXEC,
};

/* Agent state. */
enum clm_agent_state {
	CLM_STATE_IDLE,
	CLM_STATE_THINKING,
	CLM_STATE_CALLING_TOOL,
	CLM_STATE_COMPLETE,
	CLM_STATE_ERROR,
};

/*
 * Agent configuration. All string fields are borrowed: the caller retains
 * ownership and must keep them valid for the lifetime of the agent.
 */
struct clm_cfg {
	const char *api_key;
	const char *base_url;
	enum clm_provider provider;
	const char *model;
	size_t max_iterations;
};

CLM_API int clm_agent_new(const struct clm_cfg *cfg, struct clm_agent **out);
CLM_API void clm_agent_free(struct clm_agent *agent);
CLM_API int clm_agent_run(struct clm_agent *agent, const char *prompt, char **result);
CLM_API enum clm_agent_state clm_agent_get_state(const struct clm_agent *agent);
CLM_API const char *clm_agent_get_last_error(const struct clm_agent *agent);

CLM_API int clm_tool_register(struct clm_agent *agent, enum clm_tool_type type, const char *name);

CLM_API void clm_agent_free_ptr(struct clm_agent **agent);
#define _cleanup_clm_ __attribute__((cleanup(clm_agent_free_ptr)))

#endif /* CLM_H */
