// SPDX-License-Identifier: ISC
/*
 * OpenAI Responses API provider ops (see clm/provider.h for the seam this
 * fills). Newer OpenAI models (e.g. gpt-5.6-luna, gpt-5.6-sol) reject tool
 * calls on /v1/chat/completions when the backend injects a reasoning_effort
 * parameter incompatible with that endpoint; OpenAI's own guidance is to use
 * /v1/responses for function tools with these models. This translates the
 * canonical chat-completions-shaped request/response/stream (see
 * clm/provider.h) to and from that API's structurally different shape:
 *
 *   - request:            {model, input, stream, tools, ...}
 *   - non-streaming reply: output[] array of typed items (message,
 *                          function_call, reasoning) instead of choices[]
 *   - streaming reply:     response.output_text.delta /
 *                          response.function_call_arguments.delta /
 *                          response.completed events instead of
 *                          choices[].delta chunks
 *
 * Tool definitions are flat ({type:"function", name, description,
 * parameters}, no nested "function" object) and tool results are fed back
 * as {type:"function_call_output", call_id, output} input items rather than
 * a "tool" role message -- both translated at build_request() the same way
 * provider_anthropic.c translates its own structural differences.
 *
 * TODO: this still resends the full canonical history every turn (matching
 * clm's existing stateless architecture -- see clm_history_to_json), rather
 * than using the Responses API's native server-side conversation state
 * (previous_response_id). Wiring that up would let clm send only the new
 * turn's input items and skip re-transmitting history the server already
 * has, but needs agent.c to track a per-turn response id and support a
 * stateful path alongside the stateless one every other provider uses --
 * a separate, more invasive change than this file.
 */
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "clm/cleanup.h"
#include "clm/provider.h"
#include "banned.h"

/* Convert one canonical tool_calls[] entry ({id, type, function:{name,
 * arguments}}) into a Responses API function_call input item -- the shape
 * this API expects when replaying an assistant turn that made tool calls
 * back into the input array (so the model sees its own prior call). */
static cJSON *
tool_call_to_function_call_item(const cJSON *tc)
{
	cJSON *item, *func, *jid, *jname, *jargs;

	item = cJSON_CreateObject();
	if (item == NULL)
		return NULL;

	cJSON_AddItemToObject(item, "type", cJSON_CreateString("function_call"));

	jid = cJSON_GetObjectItemCaseSensitive(tc, "id");
	cJSON_AddItemToObject(item, "call_id",
	    cJSON_CreateString(jid != NULL && cJSON_IsString(jid) ? jid->valuestring : ""));

	func = cJSON_GetObjectItemCaseSensitive(tc, "function");
	jname = func ? cJSON_GetObjectItemCaseSensitive(func, "name") : NULL;
	cJSON_AddItemToObject(item, "name",
	    cJSON_CreateString(jname != NULL && cJSON_IsString(jname) ? jname->valuestring : ""));

	jargs = func ? cJSON_GetObjectItemCaseSensitive(func, "arguments") : NULL;
	cJSON_AddItemToObject(item, "arguments",
	    cJSON_CreateString(jargs != NULL && cJSON_IsString(jargs) ? jargs->valuestring : "{}"));

	return item;
}

/*
 * Translate the canonical messages[] array into a Responses API input[]
 * array. Takes ownership of `messages` (always deletes it). Returns a new
 * array on success, or NULL on OOM/malformed input.
 *
 * system  -> input item {role:"system", content:text}
 * user    -> input item {role:"user", content:text}
 * assistant, no tool_calls -> {role:"assistant", content:text}
 * assistant, tool_calls    -> one function_call item per call (text, if
 *                             any, is dropped -- Responses API has no slot
 *                             for "text alongside tool calls" on replay;
 *                             the original response already delivered it)
 * tool    -> {type:"function_call_output", call_id, output}
 */
static cJSON *
convert_messages(cJSON *messages)
{
	json_cleanup cJSON *in = messages;
	cJSON *out;
	int i, n;

	out = cJSON_CreateArray();
	if (out == NULL)
		return NULL;

	n = cJSON_GetArraySize(in);
	for (i = 0; i < n; i++) {
		cJSON *m = cJSON_GetArrayItem(in, i);
		cJSON *jrole = m ? cJSON_GetObjectItemCaseSensitive(m, "role") : NULL;
		cJSON *jcontent = m ? cJSON_GetObjectItemCaseSensitive(m, "content") : NULL;
		const char *role = cJSON_IsString(jrole) ? jrole->valuestring : "";
		const char *content = cJSON_IsString(jcontent) ? jcontent->valuestring : NULL;

		if (strcmp(role, "tool") == 0) {
			cJSON *jtid = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
			cJSON *item = cJSON_CreateObject();

			if (item == NULL) {
				cJSON_Delete(out);
				return NULL;
			}
			cJSON_AddItemToObject(item, "type", cJSON_CreateString("function_call_output"));
			cJSON_AddItemToObject(item, "call_id",
			    cJSON_CreateString(cJSON_IsString(jtid) ? jtid->valuestring : ""));
			cJSON_AddItemToObject(item, "output", cJSON_CreateString(content ? content : ""));
			cJSON_AddItemToArray(out, item);
			continue;
		}

		{
			cJSON *tool_calls = m ? cJSON_GetObjectItemCaseSensitive(m, "tool_calls") : NULL;

			if (strcmp(role, "assistant") == 0 && cJSON_IsArray(tool_calls) &&
			    cJSON_GetArraySize(tool_calls) > 0) {
				int j, m2 = cJSON_GetArraySize(tool_calls);

				for (j = 0; j < m2; j++) {
					cJSON *tc = cJSON_GetArrayItem(tool_calls, j);
					cJSON *item = tc ? tool_call_to_function_call_item(tc) : NULL;

					if (item != NULL)
						cJSON_AddItemToArray(out, item);
				}
				continue;
			}

			{
				cJSON *item = cJSON_CreateObject();

				if (item == NULL) {
					cJSON_Delete(out);
					return NULL;
				}
				cJSON_AddItemToObject(item, "role", cJSON_CreateString(role));
				cJSON_AddItemToObject(item, "content", cJSON_CreateString(content ? content : ""));
				cJSON_AddItemToArray(out, item);
			}
		}
	}

	return out;
}

/* Convert the canonical OpenAI chat-completions tools[] array ({type:
 * "function", function:{name, description, parameters}}) into the Responses
 * API's flat shape ({type:"function", name, description, parameters}, no
 * nested "function" object). Takes ownership of `tools`. */
static cJSON *
convert_tools(cJSON *tools)
{
	json_cleanup cJSON *in = tools;
	cJSON *out;
	int i, n;

	out = cJSON_CreateArray();
	if (out == NULL)
		return NULL;

	n = cJSON_GetArraySize(in);
	for (i = 0; i < n; i++) {
		cJSON *t = cJSON_GetArrayItem(in, i);
		cJSON *func = t ? cJSON_GetObjectItemCaseSensitive(t, "function") : NULL;
		cJSON *jname, *jdesc, *params, *out_t;

		if (func == NULL)
			continue;
		out_t = cJSON_CreateObject();
		if (out_t == NULL) {
			cJSON_Delete(out);
			return NULL;
		}
		cJSON_AddItemToObject(out_t, "type", cJSON_CreateString("function"));
		jname = cJSON_GetObjectItemCaseSensitive(func, "name");
		jdesc = cJSON_GetObjectItemCaseSensitive(func, "description");
		cJSON_AddItemToObject(out_t, "name",
		    cJSON_CreateString(cJSON_IsString(jname) ? jname->valuestring : ""));
		cJSON_AddItemToObject(out_t, "description",
		    cJSON_CreateString(cJSON_IsString(jdesc) ? jdesc->valuestring : ""));

		/* Detach (not duplicate) -- this function owns `in` outright. */
		params = cJSON_DetachItemFromObjectCaseSensitive(func, "parameters");
		cJSON_AddItemToObject(out_t, "parameters",
		    params != NULL ? params : cJSON_CreateObject());

		cJSON_AddItemToArray(out, out_t);
	}

	return out;
}

static cJSON *
responses_build_request(const struct clm_llm *llm, cJSON *messages, cJSON *tools, bool stream)
{
	json_cleanup cJSON *req = NULL;
	cJSON *input, *rtools;

	input = convert_messages(messages);
	if (input == NULL) {
		cJSON_Delete(tools);
		return NULL;
	}

	req = cJSON_CreateObject();
	if (req == NULL) {
		cJSON_Delete(input);
		cJSON_Delete(tools);
		return NULL;
	}

	cJSON_AddItemToObject(req, "model", cJSON_CreateString(llm->model));
	cJSON_AddItemToObject(req, "input", input);
	cJSON_AddItemToObject(req, "stream", cJSON_CreateBool(stream));

	/* Some current-generation models (e.g. gpt-5.6-luna/sol) reject
	 * function tools on chat/completions when a reasoning_effort default
	 * gets attached server-side; the same models accept tools cleanly on
	 * /v1/responses with no reasoning_effort of clm's own -- omit that
	 * field entirely and let the model/endpoint pick its own default
	 * rather than clm asserting an opinion it has no config surface for. */

	if (tools != NULL) {
		rtools = convert_tools(tools);
		if (rtools == NULL)
			return NULL;
		if (cJSON_GetArraySize(rtools) > 0) {
			cJSON *tool_choice;

			cJSON_AddItemToObject(req, "tools", rtools);
			/* Mirror the chat-completions ops' handling -- only
			 * serialize tool dispatch when the caller actually
			 * needs it (see clm_cfg.disable_parallel_tool_calls
			 * and provider_openai.c for the full rationale).
			 * Responses API's equivalent knob is top-level
			 * "parallel_tool_calls". */
			if (llm->disable_parallel_tool_calls)
				cJSON_AddItemToObject(req, "parallel_tool_calls", cJSON_CreateBool(0));
			tool_choice = cJSON_CreateString("auto");
			if (tool_choice != NULL)
				cJSON_AddItemToObject(req, "tool_choice", tool_choice);
		} else {
			cJSON_Delete(rtools);
		}
	}

	{
		cJSON *ret = req;
		req = NULL;
		return ret;
	}
}

static const char *
map_status(const char *status, bool has_tool_calls)
{
	if (has_tool_calls)
		return "tool_calls";
	if (status == NULL)
		return NULL;
	if (strcmp(status, "completed") == 0)
		return "stop";
	if (strcmp(status, "incomplete") == 0)
		return "length";
	if (strcmp(status, "failed") == 0 || strcmp(status, "cancelled") == 0)
		return "content_filter";
	return status;
}

static cJSON *
responses_normalize_response(cJSON *raw)
{
	json_cleanup cJSON *in = raw;
	cJSON *output, *jstatus, *jusage;
	autofree char *text_buf = NULL, *reasoning_buf = NULL;
	cJSON *tool_calls = NULL;
	cJSON *out, *choices, *choice0, *message, *usage;
	int i, n;

	output = cJSON_GetObjectItemCaseSensitive(in, "output");
	if (!cJSON_IsArray(output))
		return NULL;

	n = cJSON_GetArraySize(output);
	for (i = 0; i < n; i++) {
		cJSON *item = cJSON_GetArrayItem(output, i);
		cJSON *jtype = item ? cJSON_GetObjectItemCaseSensitive(item, "type") : NULL;
		const char *itype = cJSON_IsString(jtype) ? jtype->valuestring : "";

		if (strcmp(itype, "message") == 0) {
			cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
			int j, cn = cJSON_IsArray(content) ? cJSON_GetArraySize(content) : 0;

			for (j = 0; j < cn; j++) {
				cJSON *c = cJSON_GetArrayItem(content, j);
				cJSON *jctype = c ? cJSON_GetObjectItemCaseSensitive(c, "type") : NULL;
				cJSON *jtext = c ? cJSON_GetObjectItemCaseSensitive(c, "text") : NULL;

				if (cJSON_IsString(jctype) &&
				    strcmp(jctype->valuestring, "output_text") == 0 &&
				    cJSON_IsString(jtext)) {
					size_t old = text_buf ? strlen(text_buf) : 0;
					size_t add = strlen(jtext->valuestring);
					char *p = realloc(text_buf, old + add + 1);
					if (p == NULL)
						return NULL;
					memcpy(p + old, jtext->valuestring, add + 1);
					text_buf = p;
				}
			}
		} else if (strcmp(itype, "reasoning") == 0) {
			cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
			int j, sn = cJSON_IsArray(summary) ? cJSON_GetArraySize(summary) : 0;

			for (j = 0; j < sn; j++) {
				cJSON *s = cJSON_GetArrayItem(summary, j);
				cJSON *jtext = s ? cJSON_GetObjectItemCaseSensitive(s, "text") : NULL;

				if (cJSON_IsString(jtext)) {
					size_t old = reasoning_buf ? strlen(reasoning_buf) : 0;
					size_t add = strlen(jtext->valuestring);
					char *p = realloc(reasoning_buf, old + add + 1);
					if (p == NULL)
						return NULL;
					memcpy(p + old, jtext->valuestring, add + 1);
					reasoning_buf = p;
				}
			}
		} else if (strcmp(itype, "function_call") == 0) {
			cJSON *jcid = cJSON_GetObjectItemCaseSensitive(item, "call_id");
			cJSON *jname = cJSON_GetObjectItemCaseSensitive(item, "name");
			cJSON *jargs = cJSON_GetObjectItemCaseSensitive(item, "arguments");
			cJSON *call, *func;

			if (tool_calls == NULL) {
				tool_calls = cJSON_CreateArray();
				if (tool_calls == NULL)
					return NULL;
			}
			call = cJSON_CreateObject();
			if (call == NULL)
				return NULL;
			cJSON_AddItemToArray(tool_calls, call);
			cJSON_AddItemToObject(call, "id",
			    cJSON_CreateString(cJSON_IsString(jcid) ? jcid->valuestring : ""));
			cJSON_AddItemToObject(call, "type", cJSON_CreateString("function"));
			func = cJSON_CreateObject();
			if (func == NULL)
				return NULL;
			cJSON_AddItemToObject(call, "function", func);
			cJSON_AddItemToObject(func, "name",
			    cJSON_CreateString(cJSON_IsString(jname) ? jname->valuestring : ""));
			cJSON_AddItemToObject(func, "arguments",
			    cJSON_CreateString(cJSON_IsString(jargs) ? jargs->valuestring : "{}"));
		}
	}

	out = cJSON_CreateObject();
	if (out == NULL) {
		cJSON_Delete(tool_calls);
		return NULL;
	}
	choices = cJSON_CreateArray();
	choice0 = cJSON_CreateObject();
	message = cJSON_CreateObject();
	if (choices == NULL || choice0 == NULL || message == NULL) {
		cJSON_Delete(out);
		cJSON_Delete(choices);
		cJSON_Delete(choice0);
		cJSON_Delete(message);
		cJSON_Delete(tool_calls);
		return NULL;
	}
	cJSON_AddItemToObject(out, "choices", choices);
	cJSON_AddItemToArray(choices, choice0);
	cJSON_AddItemToObject(choice0, "message", message);

	cJSON_AddItemToObject(message, "role", cJSON_CreateString("assistant"));
	if (text_buf != NULL)
		cJSON_AddItemToObject(message, "content", cJSON_CreateString(text_buf));
	else
		cJSON_AddItemToObject(message, "content", cJSON_CreateNull());
	if (reasoning_buf != NULL)
		cJSON_AddItemToObject(message, "reasoning_content", cJSON_CreateString(reasoning_buf));
	if (tool_calls != NULL)
		cJSON_AddItemToObject(message, "tool_calls", tool_calls);

	jstatus = cJSON_GetObjectItemCaseSensitive(in, "status");
	{
		const char *mapped = map_status(cJSON_IsString(jstatus) ? jstatus->valuestring : NULL,
		    tool_calls != NULL);
		if (mapped != NULL)
			cJSON_AddItemToObject(choice0, "finish_reason", cJSON_CreateString(mapped));
	}

	jusage = cJSON_GetObjectItemCaseSensitive(in, "usage");
	if (cJSON_IsObject(jusage)) {
		cJSON *jin = cJSON_GetObjectItemCaseSensitive(jusage, "input_tokens");
		cJSON *jout = cJSON_GetObjectItemCaseSensitive(jusage, "output_tokens");
		double itok = cJSON_IsNumber(jin) ? jin->valuedouble : 0;
		double otok = cJSON_IsNumber(jout) ? jout->valuedouble : 0;

		usage = cJSON_CreateObject();
		if (usage != NULL) {
			cJSON_AddItemToObject(usage, "prompt_tokens", cJSON_CreateNumber(itok));
			cJSON_AddItemToObject(usage, "completion_tokens", cJSON_CreateNumber(otok));
			cJSON_AddItemToObject(usage, "total_tokens", cJSON_CreateNumber(itok + otok));
			cJSON_AddItemToObject(out, "usage", usage);
		}
	}

	return out;
}

/* Per-turn scratch state: tracks whether any function_call output item has
 * been seen (to report finish_reason "tool_calls" instead of the response's
 * own "completed" status, mirroring the non-streaming path) and each
 * function_call item's declared output_index -> call_id/name, since
 * response.function_call_arguments.delta events don't repeat them. */
struct resp_stream_state {
	bool saw_tool_call;
};

static struct resp_stream_state *
stream_state(void **state)
{
	if (*state == NULL)
		*state = calloc(1, sizeof(struct resp_stream_state));
	return *state;
}

static cJSON *
make_delta_chunk(const char *key, const char *value)
{
	cJSON *out = cJSON_CreateObject();
	cJSON *choices, *choice0, *delta;

	if (out == NULL)
		return NULL;
	choices = cJSON_CreateArray();
	choice0 = cJSON_CreateObject();
	delta = cJSON_CreateObject();
	if (choices == NULL || choice0 == NULL || delta == NULL) {
		cJSON_Delete(out);
		cJSON_Delete(choices);
		cJSON_Delete(choice0);
		cJSON_Delete(delta);
		return NULL;
	}
	cJSON_AddItemToObject(out, "choices", choices);
	cJSON_AddItemToArray(choices, choice0);
	cJSON_AddItemToObject(choice0, "delta", delta);
	if (key != NULL)
		cJSON_AddItemToObject(delta, key, cJSON_CreateString(value ? value : ""));
	return out;
}

/* Build a canonical delta chunk carrying one tool_calls[] entry at `index`,
 * with optional id/name (first mention) and/or an arguments fragment. */
static cJSON *
make_tool_delta_chunk(int index, const char *id, const char *name, const char *args_frag)
{
	cJSON *out, *choices, *choice0, *delta, *tool_calls, *tc, *func;

	out = cJSON_CreateObject();
	choices = cJSON_CreateArray();
	choice0 = cJSON_CreateObject();
	delta = cJSON_CreateObject();
	tool_calls = cJSON_CreateArray();
	tc = cJSON_CreateObject();
	func = cJSON_CreateObject();
	if (out == NULL || choices == NULL || choice0 == NULL || delta == NULL ||
	    tool_calls == NULL || tc == NULL || func == NULL) {
		cJSON_Delete(out);
		cJSON_Delete(choices);
		cJSON_Delete(choice0);
		cJSON_Delete(delta);
		cJSON_Delete(tool_calls);
		cJSON_Delete(tc);
		cJSON_Delete(func);
		return NULL;
	}
	cJSON_AddItemToObject(out, "choices", choices);
	cJSON_AddItemToArray(choices, choice0);
	cJSON_AddItemToObject(choice0, "delta", delta);
	cJSON_AddItemToObject(delta, "tool_calls", tool_calls);
	cJSON_AddItemToArray(tool_calls, tc);
	cJSON_AddItemToObject(tc, "index", cJSON_CreateNumber(index));
	if (id != NULL)
		cJSON_AddItemToObject(tc, "id", cJSON_CreateString(id));
	cJSON_AddItemToObject(tc, "function", func);
	if (name != NULL)
		cJSON_AddItemToObject(func, "name", cJSON_CreateString(name));
	cJSON_AddItemToObject(func, "arguments", cJSON_CreateString(args_frag ? args_frag : ""));
	return out;
}

static cJSON *
responses_normalize_stream_event(cJSON *raw, void **state)
{
	cJSON *jtype = cJSON_GetObjectItemCaseSensitive(raw, "type");
	const char *type = cJSON_IsString(jtype) ? jtype->valuestring : "";
	struct resp_stream_state *st;

	if (strcmp(type, "response.output_text.delta") == 0) {
		cJSON *jdelta = cJSON_GetObjectItemCaseSensitive(raw, "delta");
		return make_delta_chunk("content", cJSON_IsString(jdelta) ? jdelta->valuestring : "");
	}

	if (strcmp(type, "response.reasoning_summary_text.delta") == 0) {
		cJSON *jdelta = cJSON_GetObjectItemCaseSensitive(raw, "delta");
		return make_delta_chunk("reasoning_content", cJSON_IsString(jdelta) ? jdelta->valuestring : "");
	}

	if (strcmp(type, "response.output_item.added") == 0) {
		cJSON *item = cJSON_GetObjectItemCaseSensitive(raw, "item");
		cJSON *jitype = item ? cJSON_GetObjectItemCaseSensitive(item, "type") : NULL;
		cJSON *jindex = cJSON_GetObjectItemCaseSensitive(raw, "output_index");
		int index = cJSON_IsNumber(jindex) ? (int)jindex->valuedouble : 0;

		if (item != NULL && cJSON_IsString(jitype) &&
		    strcmp(jitype->valuestring, "function_call") == 0) {
			cJSON *jcid = cJSON_GetObjectItemCaseSensitive(item, "call_id");
			cJSON *jname = cJSON_GetObjectItemCaseSensitive(item, "name");

			st = stream_state(state);
			if (st != NULL)
				st->saw_tool_call = true;
			return make_tool_delta_chunk(index,
			    cJSON_IsString(jcid) ? jcid->valuestring : "",
			    cJSON_IsString(jname) ? jname->valuestring : "", NULL);
		}
		return NULL;
	}

	if (strcmp(type, "response.function_call_arguments.delta") == 0) {
		cJSON *jindex = cJSON_GetObjectItemCaseSensitive(raw, "output_index");
		cJSON *jdelta = cJSON_GetObjectItemCaseSensitive(raw, "delta");
		int index = cJSON_IsNumber(jindex) ? (int)jindex->valuedouble : 0;

		return make_tool_delta_chunk(index, NULL, NULL,
		    cJSON_IsString(jdelta) ? jdelta->valuestring : "");
	}

	if (strcmp(type, "response.completed") == 0 || strcmp(type, "response.incomplete") == 0 ||
	    strcmp(type, "response.failed") == 0) {
		cJSON *response = cJSON_GetObjectItemCaseSensitive(raw, "response");
		cJSON *jstatus = response ? cJSON_GetObjectItemCaseSensitive(response, "status") : NULL;
		cJSON *jusage = response ? cJSON_GetObjectItemCaseSensitive(response, "usage") : NULL;
		cJSON *out, *choices, *choice0;
		const char *mapped;

		st = stream_state(state);
		out = cJSON_CreateObject();
		choices = cJSON_CreateArray();
		choice0 = cJSON_CreateObject();
		if (out == NULL || choices == NULL || choice0 == NULL) {
			cJSON_Delete(out);
			cJSON_Delete(choices);
			cJSON_Delete(choice0);
			return NULL;
		}
		cJSON_AddItemToObject(out, "choices", choices);
		cJSON_AddItemToArray(choices, choice0);

		mapped = map_status(cJSON_IsString(jstatus) ? jstatus->valuestring : NULL,
		    st != NULL && st->saw_tool_call);
		if (mapped != NULL)
			cJSON_AddItemToObject(choice0, "finish_reason", cJSON_CreateString(mapped));

		if (cJSON_IsObject(jusage)) {
			cJSON *jin = cJSON_GetObjectItemCaseSensitive(jusage, "input_tokens");
			cJSON *jout = cJSON_GetObjectItemCaseSensitive(jusage, "output_tokens");
			double itok = cJSON_IsNumber(jin) ? jin->valuedouble : 0;
			double otok = cJSON_IsNumber(jout) ? jout->valuedouble : 0;
			cJSON *cu = cJSON_CreateObject();

			if (cu != NULL) {
				cJSON_AddItemToObject(cu, "prompt_tokens", cJSON_CreateNumber(itok));
				cJSON_AddItemToObject(cu, "completion_tokens", cJSON_CreateNumber(otok));
				cJSON_AddItemToObject(cu, "total_tokens", cJSON_CreateNumber(itok + otok));
				cJSON_AddItemToObject(out, "usage", cu);
			}
		}
		return out;
	}

	/* response.created, response.in_progress, response.output_item.done,
	 * response.content_part.added/done, error, etc: nothing to merge. */
	return NULL;
}

const struct clm_provider_ops clm_provider_ops_responses = {
	.build_request = responses_build_request,
	.build_auth_headers = NULL,
	.normalize_response = responses_normalize_response,
	.normalize_stream_event = responses_normalize_stream_event,
	.endpoint_path = "responses",
};
