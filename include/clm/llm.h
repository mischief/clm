// SPDX-License-Identifier: ISC
#ifndef CLM_LLM_H
#define CLM_LLM_H

#include <stddef.h>

#include <json-c/json.h>

#include "clm/clm.h"
#include "clm/http.h"

struct clm_llm {
	enum clm_provider provider;
	char *api_key;
	char *base_url;
	char *model;
};

int clm_llm_new(struct clm_llm **ret, enum clm_provider provider, const char *api_key, const char *base_url, const char *model);
void clm_llm_free(struct clm_llm *llm);

/*
 * Issue a chat completion. messages is a json array (borrowed) in the
 * OpenAI "messages" shape; tools is a json array (borrowed) of tool schemas
 * or NULL. On success resp holds the raw HTTP response body; the caller frees
 * it with clm_http_response_free.
 */
int clm_llm_chat(struct clm_llm *llm, struct json_object *messages,
    struct json_object *tools, struct clm_http_response *resp);

#endif /* CLM_LLM_H */
