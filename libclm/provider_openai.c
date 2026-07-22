// SPDX-License-Identifier: ISC
/*
 * OpenAI-compatible provider ops: the wire format clm's internal
 * representation already is (see clm/provider.h), so build_request is the
 * only real work -- assembling the fields around the canonical
 * messages/tools arrays -- and every response-side hook is NULL ("already
 * canonical, no translation needed").
 */
#include <stdlib.h>

#include <cjson/cJSON.h>

#include "clm/provider.h"
#include "banned.h"

static cJSON *
openai_build_request(const struct clm_llm *llm, cJSON *messages, cJSON *tools, bool stream)
{
	cJSON *req, *jmodel, *jstream;

	req = cJSON_CreateObject();
	if (req == NULL)
		goto fail;

	jmodel = cJSON_CreateString(llm->model);
	if (jmodel == NULL)
		goto fail;
	cJSON_AddItemToObject(req, "model", jmodel);

	cJSON_AddItemToObject(req, "messages", messages);
	messages = NULL; /* req owns it now, even if a later step fails */

	jstream = cJSON_CreateBool(stream);
	if (jstream == NULL)
		goto fail;
	cJSON_AddItemToObject(req, "stream", jstream);

	/* Ask the server to include token usage in the final stream chunk. */
	if (stream) {
		cJSON *so = cJSON_CreateObject();
		if (so != NULL) {
			cJSON *inc = cJSON_CreateBool(1);
			if (inc != NULL)
				cJSON_AddItemToObject(so, "include_usage", inc);
			cJSON_AddItemToObject(req, "stream_options", so);
		}
	}

	/* tools is NULL when the caller (clm_agent_start_turn) already knows
	 * this model/provider doesn't support tool calls (see
	 * agent->tools_unsupported) -- omit both fields entirely rather than
	 * sending an empty "tools":[] plus "parallel_tool_calls", which is not
	 * the same signal to every backend as never mentioning tools at all
	 * and some backends reject "parallel_tool_calls" without "tools"
	 * present. */
	if (tools != NULL) {
		cJSON_AddItemToObject(req, "tools", tools);
		tools = NULL; /* req owns it now, even if a later step fails */

		/* Only send parallel_tool_calls:false when the caller (see
		 * clm_cfg.disable_parallel_tool_calls) actually needs serial
		 * dispatch -- a tool host that can only process one action at
		 * a time (e.g. a game bridge advancing one action per game
		 * turn) deadlocks on clm's own concurrent batch dispatch (see
		 * clm_tools_dispatch) otherwise. Omitted rather than sent as
		 * true when not needed, to match the API's own default and
		 * let the model batch tool calls as it likes. */
		if (llm->disable_parallel_tool_calls)
			cJSON_AddItemToObject(req, "parallel_tool_calls", cJSON_CreateBool(0));
	}

	return req;

fail:
	cJSON_Delete(req);
	cJSON_Delete(messages);
	cJSON_Delete(tools);
	return NULL;
}

const struct clm_provider_ops clm_provider_ops_openai = {
	.build_request = openai_build_request,
	.build_auth_headers = NULL,
	.normalize_response = NULL,
	.normalize_stream_event = NULL,
	.endpoint_path = "chat/completions",
};
