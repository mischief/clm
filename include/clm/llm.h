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
};

int clm_llm_new(struct clm_llm **ret, enum clm_provider provider, const char *api_key, const char *base_url, const char *model, bool disable_parallel_tool_calls);
void clm_llm_free(struct clm_llm *llm);

#endif /* CLM_LLM_H */
