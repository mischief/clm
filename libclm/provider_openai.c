// SPDX-License-Identifier: ISC
/*
 * OpenAI-compatible provider ops: the wire format clm's internal
 * representation already is (see clm/provider.h), so build_request is the
 * only real work -- assembling the fields around the canonical
 * messages/tools arrays -- and every response-side hook is NULL ("already
 * canonical, no translation needed").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "clm/provider.h"
#include "banned.h"

/* The name of the tool call id belongs to, from the assistant message
 * before index i that requested it. */
static const char *
tool_name_for(cJSON *messages, int i, const char *id)
{
	for (i--; i >= 0; i--) {
		cJSON *calls = cJSON_GetObjectItemCaseSensitive(
		    cJSON_GetArrayItem(messages, i), "tool_calls");
		cJSON *c;

		cJSON_ArrayForEach(c, calls)
		{
			const char *cid = cJSON_GetStringValue(
			    cJSON_GetObjectItemCaseSensitive(c, "id"));

			if (cid != NULL && id != NULL && strcmp(cid, id) == 0)
				return cJSON_GetStringValue(
				    cJSON_GetObjectItemCaseSensitive(
				        cJSON_GetObjectItemCaseSensitive(
				            c, "function"),
				        "name"));
		}
	}
	return NULL;
}

/*
 * Move the images of one tool message into carrier, labelled, and leave
 * the message its text as a plain string.
 */
static int
split_tool_images(cJSON *messages, int i, cJSON *msg, cJSON *carrier)
{
	cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
	const char *id = cJSON_GetStringValue(
	    cJSON_GetObjectItemCaseSensitive(msg, "tool_call_id"));
	const char *name = tool_name_for(messages, i, id);
	char *text = NULL;
	size_t tlen = 0;
	cJSON *part, *next;

	for (part = content->child; part != NULL; part = next) {
		const char *type = cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(part, "type"));

		next = part->next;
		if (type != NULL && strcmp(type, "text") == 0) {
			const char *t = cJSON_GetStringValue(
			    cJSON_GetObjectItemCaseSensitive(part, "text"));
			size_t n = t != NULL ? strlen(t) : 0;
			char *nt = realloc(text, tlen + n + 2);

			if (nt == NULL) {
				free(text);
				return -1;
			}
			text = nt;
			if (tlen > 0)
				text[tlen++] = '\n';
			memcpy(text + tlen, t != NULL ? t : "", n);
			tlen += n;
			text[tlen] = '\0';
		} else if (type != NULL && strcmp(type, "image_url") == 0) {
			char label[256];
			cJSON *lp = cJSON_CreateObject();

			(void)snprintf(label, sizeof(label),
			    "image from tool %s (call %s):",
			    name != NULL ? name : "?", id != NULL ? id : "?");
			if (lp == NULL ||
			    cJSON_AddStringToObject(lp, "type", "text") ==
			        NULL ||
			    cJSON_AddStringToObject(lp, "text", label) ==
			        NULL) {
				cJSON_Delete(lp);
				free(text);
				return -1;
			}
			cJSON_AddItemToArray(carrier, lp);
			cJSON_AddItemToArray(
			    carrier, cJSON_DetachItemViaPointer(content, part));
		}
	}
	cJSON_DeleteItemFromObjectCaseSensitive(msg, "content");
	cJSON_AddItemToObject(msg, "content",
	    cJSON_CreateString(text != NULL ? text : "(image result)"));
	free(text);
	return 0;
}

/*
 * Chat completions reads only text from a tool message: OpenAI accepts
 * image parts there and silently ignores them. So the images of a batch of
 * tool results go in one user message right after the batch; a tool
 * message must follow its assistant tool_calls directly, so the carrier
 * never goes between them.
 */
static int
carry_tool_images(cJSON *messages)
{
	cJSON *carrier = NULL;
	int n = cJSON_GetArraySize(messages);

	for (int i = 0; i <= n; i++) {
		cJSON *msg = i < n ? cJSON_GetArrayItem(messages, i) : NULL;
		const char *role = cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(msg, "role"));
		bool tool = role != NULL && strcmp(role, "tool") == 0;

		if (tool &&
		    cJSON_IsArray(
		        cJSON_GetObjectItemCaseSensitive(msg, "content"))) {
			if (carrier == NULL &&
			    (carrier = cJSON_CreateArray()) == NULL)
				return -1;
			if (split_tool_images(messages, i, msg, carrier) < 0) {
				cJSON_Delete(carrier);
				return -1;
			}
			continue;
		}
		if (tool || carrier == NULL)
			continue;
		/* The batch ended before message i: insert the carrier. */
		{
			cJSON *user = cJSON_CreateObject();

			if (user == NULL ||
			    cJSON_AddStringToObject(user, "role", "user") ==
			        NULL) {
				cJSON_Delete(user);
				cJSON_Delete(carrier);
				return -1;
			}
			cJSON_AddItemToObject(user, "content", carrier);
			carrier = NULL;
			if (i < n)
				cJSON_InsertItemInArray(messages, i, user);
			else
				cJSON_AddItemToArray(messages, user);
			n++;
			i++;
		}
	}
	return 0;
}

static cJSON *
openai_build_request(
    const struct clm_llm *llm, cJSON *messages, cJSON *tools, bool stream)
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
	if (carry_tool_images(
	        cJSON_GetObjectItemCaseSensitive(req, "messages")) < 0)
		goto fail;

	jstream = cJSON_CreateBool(stream);
	if (jstream == NULL)
		goto fail;
	cJSON_AddItemToObject(req, "stream", jstream);

	/* Chat-completions spells reasoning effort as a top-level string.
	 * Backends without the notion (llama.cpp, most local servers) accept
	 * and ignore it. */
	if (llm->effort != NULL) {
		cJSON *je = cJSON_CreateString(llm->effort);

		if (je != NULL)
			cJSON_AddItemToObject(req, "reasoning_effort", je);
	}

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
			cJSON_AddItemToObject(
			    req, "parallel_tool_calls", cJSON_CreateBool(0));
	}

	return req;

fail:
	cJSON_Delete(req);
	cJSON_Delete(messages);
	cJSON_Delete(tools);
	return NULL;
}

/* Both OpenAI dialects take the bare string. */
static void
openai_forbid_tool_calls(cJSON *req)
{
	cJSON *tc = cJSON_CreateString("none");

	if (tc == NULL)
		return;
	cJSON_DeleteItemFromObjectCaseSensitive(req, "tool_choice");
	cJSON_AddItemToObject(req, "tool_choice", tc);
}

const struct clm_provider_ops clm_provider_ops_openai = {
    .forbid_tool_calls = openai_forbid_tool_calls,
    .build_request = openai_build_request,
    .build_auth_headers = NULL,
    .normalize_response = NULL,
    .normalize_stream_event = NULL,
    .endpoint_path = "chat/completions",
};
