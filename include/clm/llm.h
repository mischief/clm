// SPDX-License-Identifier: ISC
#ifndef CLM_LLM_H
#define CLM_LLM_H

#include <stdbool.h>
#include <stddef.h>

#include "clm/clm.h"

struct clm_llm {
	enum clm_provider provider;
	char *api_key;
	char *base_url;
	char *model;
	bool disable_parallel_tool_calls;
	/* Reasoning effort as the provider spells it, or NULL for whatever
	 * the backend defaults to. Set through clm_agent_set_effort(). */
	char *effort;
	/* Responses API only: the response this request continues from, so
	 * the history already on the server is not resent. Borrowed, set per
	 * request by the agent. NULL sends the whole conversation. */
	const char *prev_response_id;
};

int clm_llm_new(struct clm_llm **ret, enum clm_provider provider,
    const char *api_key, const char *base_url, const char *model,
    bool disable_parallel_tool_calls);
void clm_llm_free(struct clm_llm *llm);

/* Replace llm->effort with a copy of `effort` (NULL clears it). */
int clm_llm_set_effort(struct clm_llm *llm, const char *effort);

#endif /* CLM_LLM_H */
