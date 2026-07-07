// SPDX-License-Identifier: ISC
/*
 * Anthropic Messages API provider ops (see clm/provider.h for the seam this
 * fills). Unlike the OpenAI-compatible ops, every hook here does real work:
 * Anthropic's request shape, non-streaming response shape, and SSE event
 * model are all structurally different from the canonical
 * choices[]/messages[] shape the rest of clm works in.
 *
 * Request shape differences handled by anthropic_build_request():
 *   - no "choices"-style role for system prompts: they're pulled out of the
 *     canonical messages array into a top-level "system" string.
 *   - assistant tool calls are content blocks ({"type":"tool_use", id, name,
 *     input}) inside the message's "content" array, not a separate
 *     "tool_calls" field, and "input" is a real JSON object rather than an
 *     encoded string.
 *   - tool results are a "user" message with "tool_result" content blocks,
 *     not a dedicated "tool" role; consecutive canonical tool messages
 *     (the results of one batch of parallel tool calls) are merged into a
 *     single such user message, matching how Anthropic expects them.
 *   - "max_tokens" is required (clm has no per-request token budget config,
 *     so a fixed generous default is used -- see CLM_ANTHROPIC_MAX_TOKENS).
 *   - tool schemas use "input_schema" instead of "function.parameters".
 *
 * Response shape differences handled by anthropic_normalize_response() and
 * anthropic_normalize_stream_event(): a single Message with a "content"
 * block array (text/tool_use/thinking blocks) instead of choices[], and a
 * content_block_start/content_block_delta/message_delta event stream
 * instead of choices[].delta chunks.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "clm/cleanup.h"
#include "clm/provider.h"
#include "banned.h"

#define CLM_ANTHROPIC_VERSION "2023-06-01"

/* Anthropic requires max_tokens on every request; clm has no per-request
 * token-budget knob (see struct clm_cfg), so use a fixed, generous default
 * rather than growing new public config surface for this one provider. */
#define CLM_ANTHROPIC_MAX_TOKENS 4096

/* ------------------------------------------------------------------ */
/* Request building                                                    */
/* ------------------------------------------------------------------ */

/* Append the content of a canonical system-role message to *sys (allocating
 * or growing it), separating multiple system messages with a blank line. */
static int
append_system_text(char **sys, const char *text)
{
	size_t old_len = *sys ? strlen(*sys) : 0;
	size_t add_len = strlen(text);
	size_t sep_len = old_len > 0 ? 2 : 0;
	char *p = realloc(*sys, old_len + sep_len + add_len + 1);

	if (p == NULL)
		return -ENOMEM;
	if (sep_len > 0)
		memcpy(p + old_len, "\n\n", sep_len);
	memcpy(p + old_len + sep_len, text, add_len + 1);
	*sys = p;
	return 0;
}

/* Convert one canonical tool_calls[] entry ({id, type, function:{name,
 * arguments}}) into an Anthropic tool_use content block. */
static cJSON *
tool_call_to_tool_use(const cJSON *tc)
{
	cJSON *block, *func, *jid, *jname, *jargs, *input;

	block = cJSON_CreateObject();
	if (block == NULL)
		return NULL;

	cJSON_AddItemToObject(block, "type", cJSON_CreateString("tool_use"));

	jid = cJSON_GetObjectItemCaseSensitive(tc, "id");
	cJSON_AddItemToObject(block, "id",
	    cJSON_CreateString(jid != NULL && cJSON_IsString(jid)
	        ? jid->valuestring : ""));

	func = cJSON_GetObjectItemCaseSensitive(tc, "function");
	jname = func ? cJSON_GetObjectItemCaseSensitive(func, "name") : NULL;
	cJSON_AddItemToObject(block, "name",
	    cJSON_CreateString(jname != NULL && cJSON_IsString(jname)
	        ? jname->valuestring : ""));

	/* function.arguments is a JSON-encoded string in the canonical shape;
	 * Anthropic's "input" is the decoded object itself. */
	jargs = func ? cJSON_GetObjectItemCaseSensitive(func, "arguments") : NULL;
	input = (jargs != NULL && cJSON_IsString(jargs))
	    ? cJSON_Parse(jargs->valuestring) : NULL;
	if (input == NULL || !cJSON_IsObject(input)) {
		cJSON_Delete(input);
		input = cJSON_CreateObject();
	}
	if (input == NULL) {
		cJSON_Delete(block);
		return NULL;
	}
	cJSON_AddItemToObject(block, "input", input);

	return block;
}

/* True if `msg` is a synthetic Anthropic user message this function itself
 * built to hold merged tool_result blocks (so a run of consecutive
 * canonical "tool" messages collapses into one Anthropic message). */
static bool
is_tool_result_carrier(const cJSON *msg)
{
	cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
	cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
	cJSON *first;

	if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0)
		return false;
	if (!cJSON_IsArray(content))
		return false;
	first = cJSON_GetArrayItem(content, 0);
	if (first == NULL)
		return false;
	cJSON *type = cJSON_GetObjectItemCaseSensitive(first, "type");
	return cJSON_IsString(type) && strcmp(type->valuestring, "tool_result") == 0;
}

/*
 * Translate the canonical messages[] array into Anthropic's messages[] plus
 * a separate system string. Takes ownership of `messages` (always deletes
 * it). *system_out receives a malloc'd string (caller frees) or NULL if
 * there was no system message. Returns a new array on success, or NULL on
 * OOM/malformed input (in which case *system_out is left untouched).
 */
static cJSON *
convert_messages(cJSON *messages, char **system_out)
{
	json_cleanup cJSON *in = messages;
	autofree char *sys = NULL;
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

		if (strcmp(role, "system") == 0) {
			if (content != NULL && append_system_text(&sys, content) < 0) {
				cJSON_Delete(out);
				return NULL;
			}
			continue;
		}

		if (strcmp(role, "tool") == 0) {
			cJSON *jtid = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
			cJSON *carrier, *carr_content, *block;

			carrier = cJSON_GetArrayItem(out, cJSON_GetArraySize(out) - 1);
			if (carrier == NULL || !is_tool_result_carrier(carrier)) {
				carrier = cJSON_CreateObject();
				if (carrier == NULL) {
					cJSON_Delete(out);
					return NULL;
				}
				cJSON_AddItemToObject(carrier, "role", cJSON_CreateString("user"));
				carr_content = cJSON_CreateArray();
				if (carr_content == NULL) {
					cJSON_Delete(carrier);
					cJSON_Delete(out);
					return NULL;
				}
				cJSON_AddItemToObject(carrier, "content", carr_content);
				cJSON_AddItemToArray(out, carrier);
			} else {
				carr_content = cJSON_GetObjectItemCaseSensitive(carrier, "content");
			}

			block = cJSON_CreateObject();
			if (block == NULL) {
				cJSON_Delete(out);
				return NULL;
			}
			cJSON_AddItemToArray(carr_content, block);
			cJSON_AddItemToObject(block, "type", cJSON_CreateString("tool_result"));
			cJSON_AddItemToObject(block, "tool_use_id",
			    cJSON_CreateString(cJSON_IsString(jtid) ? jtid->valuestring : ""));
			cJSON_AddItemToObject(block, "content",
			    cJSON_CreateString(content ? content : ""));
			continue;
		}

		/* user / assistant. */
		{
			cJSON *tool_calls = m ? cJSON_GetObjectItemCaseSensitive(m, "tool_calls") : NULL;
			cJSON *out_msg = cJSON_CreateObject();

			if (out_msg == NULL) {
				cJSON_Delete(out);
				return NULL;
			}
			cJSON_AddItemToObject(out_msg, "role", cJSON_CreateString(role));

			if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
				cJSON *blocks = cJSON_CreateArray();
				cJSON *tc;
				int j, m2;

				if (blocks == NULL) {
					cJSON_Delete(out_msg);
					cJSON_Delete(out);
					return NULL;
				}
				cJSON_AddItemToObject(out_msg, "content", blocks);
				if (content != NULL && content[0] != '\0') {
					cJSON *tblock = cJSON_CreateObject();
					if (tblock != NULL) {
						cJSON_AddItemToObject(tblock, "type", cJSON_CreateString("text"));
						cJSON_AddItemToObject(tblock, "text", cJSON_CreateString(content));
						cJSON_AddItemToArray(blocks, tblock);
					}
				}
				m2 = cJSON_GetArraySize(tool_calls);
				for (j = 0; j < m2; j++) {
					tc = cJSON_GetArrayItem(tool_calls, j);
					cJSON *block = tc ? tool_call_to_tool_use(tc) : NULL;
					if (block != NULL)
						cJSON_AddItemToArray(blocks, block);
				}
			} else {
				cJSON_AddItemToObject(out_msg, "content",
				    cJSON_CreateString(content ? content : ""));
			}

			cJSON_AddItemToArray(out, out_msg);
		}
	}

	*system_out = sys;
	sys = NULL;
	return out;
}

/* Convert the canonical OpenAI-shaped tools[] array ({type:"function",
 * function:{name, description, parameters}}) into Anthropic's flatter
 * {name, description, input_schema} shape. Takes ownership of `tools`. */
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
		jname = cJSON_GetObjectItemCaseSensitive(func, "name");
		jdesc = cJSON_GetObjectItemCaseSensitive(func, "description");
		cJSON_AddItemToObject(out_t, "name",
		    cJSON_CreateString(cJSON_IsString(jname) ? jname->valuestring : ""));
		cJSON_AddItemToObject(out_t, "description",
		    cJSON_CreateString(cJSON_IsString(jdesc) ? jdesc->valuestring : ""));

		/* Detach (not duplicate) -- this function owns `in` outright and
		 * nothing else references the parameters subtree. */
		params = cJSON_DetachItemFromObjectCaseSensitive(func, "parameters");
		cJSON_AddItemToObject(out_t, "input_schema",
		    params != NULL ? params : cJSON_CreateObject());

		cJSON_AddItemToArray(out, out_t);
	}

	return out;
}

static cJSON *
anthropic_build_request(const struct clm_llm *llm, cJSON *messages, cJSON *tools, bool stream)
{
	json_cleanup cJSON *req = NULL;
	autofree char *system = NULL;
	cJSON *anth_messages, *anth_tools;

	anth_messages = convert_messages(messages, &system);
	if (anth_messages == NULL) {
		cJSON_Delete(tools);
		return NULL;
	}

	anth_tools = convert_tools(tools);
	if (anth_tools == NULL) {
		cJSON_Delete(anth_messages);
		return NULL;
	}

	req = cJSON_CreateObject();
	if (req == NULL) {
		cJSON_Delete(anth_messages);
		cJSON_Delete(anth_tools);
		return NULL;
	}

	cJSON_AddItemToObject(req, "model", cJSON_CreateString(llm->model));
	if (system != NULL)
		cJSON_AddItemToObject(req, "system", cJSON_CreateString(system));
	cJSON_AddItemToObject(req, "messages", anth_messages);
	cJSON_AddItemToObject(req, "max_tokens", cJSON_CreateNumber(CLM_ANTHROPIC_MAX_TOKENS));
	cJSON_AddItemToObject(req, "stream", cJSON_CreateBool(stream));

	if (cJSON_GetArraySize(anth_tools) > 0) {
		cJSON *tool_choice = cJSON_CreateObject();
		cJSON_AddItemToObject(req, "tools", anth_tools);
		/* Mirror the OpenAI ops' parallel_tool_calls:false -- serialize
		 * tool dispatch for hosts that can only run one action at a
		 * time (see provider_openai.c for the full rationale). */
		if (tool_choice != NULL) {
			cJSON_AddItemToObject(tool_choice, "type", cJSON_CreateString("auto"));
			cJSON_AddItemToObject(tool_choice, "disable_parallel_tool_use", cJSON_CreateBool(1));
			cJSON_AddItemToObject(req, "tool_choice", tool_choice);
		}
	} else {
		cJSON_Delete(anth_tools);
	}

	{
		cJSON *ret = req;
		req = NULL;
		return ret;
	}
}

/* ------------------------------------------------------------------ */
/* Auth headers                                                        */
/* ------------------------------------------------------------------ */

static char **
anthropic_build_auth_headers(const struct clm_llm *llm)
{
	char **headers = calloc(4, sizeof(*headers));
	size_t n = 0;

	if (headers == NULL)
		return NULL;

	headers[n] = strdup("Content-Type: application/json");
	if (headers[n] == NULL)
		goto fail;
	n++;

	if (llm->api_key != NULL && llm->api_key[0] != '\0') {
		size_t len = sizeof("x-api-key: ") + strlen(llm->api_key);
		headers[n] = malloc(len);
		if (headers[n] == NULL)
			goto fail;
		(void)snprintf(headers[n], len, "x-api-key: %s", llm->api_key);
		n++;
	}

	headers[n] = strdup("anthropic-version: " CLM_ANTHROPIC_VERSION);
	if (headers[n] == NULL)
		goto fail;
	n++;

	headers[n] = NULL;
	return headers;

fail:
	for (size_t i = 0; i < n; i++)
		free(headers[i]);
	free(headers);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Non-streaming response                                              */
/* ------------------------------------------------------------------ */

static const char *
map_stop_reason(const char *reason)
{
	if (reason == NULL)
		return NULL;
	if (strcmp(reason, "end_turn") == 0 || strcmp(reason, "stop_sequence") == 0)
		return "stop";
	if (strcmp(reason, "max_tokens") == 0)
		return "length";
	if (strcmp(reason, "tool_use") == 0)
		return "tool_calls";
	return reason;
}

static cJSON *
anthropic_normalize_response(cJSON *raw)
{
	json_cleanup cJSON *in = raw;
	cJSON *content_blocks, *jstop, *jusage;
	autofree char *text_buf = NULL, *reasoning_buf = NULL;
	cJSON *tool_calls = NULL;
	cJSON *out, *choices, *choice0, *message, *usage;
	int i, n;

	content_blocks = cJSON_GetObjectItemCaseSensitive(in, "content");
	if (!cJSON_IsArray(content_blocks))
		return NULL;

	n = cJSON_GetArraySize(content_blocks);
	for (i = 0; i < n; i++) {
		cJSON *b = cJSON_GetArrayItem(content_blocks, i);
		cJSON *jtype = b ? cJSON_GetObjectItemCaseSensitive(b, "type") : NULL;
		const char *btype = cJSON_IsString(jtype) ? jtype->valuestring : "";

		if (strcmp(btype, "text") == 0) {
			cJSON *jtext = cJSON_GetObjectItemCaseSensitive(b, "text");
			if (cJSON_IsString(jtext)) {
				size_t old = text_buf ? strlen(text_buf) : 0;
				size_t add = strlen(jtext->valuestring);
				char *p = realloc(text_buf, old + add + 1);
				if (p == NULL)
					return NULL;
				memcpy(p + old, jtext->valuestring, add + 1);
				text_buf = p;
			}
		} else if (strcmp(btype, "thinking") == 0) {
			cJSON *jthink = cJSON_GetObjectItemCaseSensitive(b, "thinking");
			if (cJSON_IsString(jthink)) {
				size_t old = reasoning_buf ? strlen(reasoning_buf) : 0;
				size_t add = strlen(jthink->valuestring);
				char *p = realloc(reasoning_buf, old + add + 1);
				if (p == NULL)
					return NULL;
				memcpy(p + old, jthink->valuestring, add + 1);
				reasoning_buf = p;
			}
		} else if (strcmp(btype, "tool_use") == 0) {
			cJSON *jid = cJSON_GetObjectItemCaseSensitive(b, "id");
			cJSON *jname = cJSON_GetObjectItemCaseSensitive(b, "name");
			cJSON *jinput = cJSON_GetObjectItemCaseSensitive(b, "input");
			autofree char *args_str = jinput ? cJSON_PrintUnformatted(jinput) : NULL;
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
			    cJSON_CreateString(cJSON_IsString(jid) ? jid->valuestring : ""));
			cJSON_AddItemToObject(call, "type", cJSON_CreateString("function"));
			func = cJSON_CreateObject();
			if (func == NULL)
				return NULL;
			cJSON_AddItemToObject(call, "function", func);
			cJSON_AddItemToObject(func, "name",
			    cJSON_CreateString(cJSON_IsString(jname) ? jname->valuestring : ""));
			cJSON_AddItemToObject(func, "arguments",
			    cJSON_CreateString(args_str ? args_str : "{}"));
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

	jstop = cJSON_GetObjectItemCaseSensitive(in, "stop_reason");
	if (cJSON_IsString(jstop)) {
		const char *mapped = map_stop_reason(jstop->valuestring);
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

/* ------------------------------------------------------------------ */
/* Streaming                                                            */
/* ------------------------------------------------------------------ */

/* Per-turn scratch state for the streaming translator: Anthropic reports
 * input tokens on message_start and output tokens (separately) on
 * message_delta, but the canonical usage shape wants both together in one
 * chunk -- so the input count is stashed here until message_delta arrives. */
struct anth_stream_state {
	double input_tokens;
};

static struct anth_stream_state *
stream_state(void **state)
{
	if (*state == NULL)
		*state = calloc(1, sizeof(struct anth_stream_state));
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
anthropic_normalize_stream_event(cJSON *raw, void **state)
{
	cJSON *jtype = cJSON_GetObjectItemCaseSensitive(raw, "type");
	const char *type = cJSON_IsString(jtype) ? jtype->valuestring : "";

	if (strcmp(type, "content_block_start") == 0) {
		cJSON *jindex = cJSON_GetObjectItemCaseSensitive(raw, "index");
		cJSON *block = cJSON_GetObjectItemCaseSensitive(raw, "content_block");
		cJSON *jbtype = block ? cJSON_GetObjectItemCaseSensitive(block, "type") : NULL;
		int index = cJSON_IsNumber(jindex) ? (int)jindex->valuedouble : 0;

		if (block != NULL && cJSON_IsString(jbtype) &&
		    strcmp(jbtype->valuestring, "tool_use") == 0) {
			cJSON *jid = cJSON_GetObjectItemCaseSensitive(block, "id");
			cJSON *jname = cJSON_GetObjectItemCaseSensitive(block, "name");
			return make_tool_delta_chunk(index,
			    cJSON_IsString(jid) ? jid->valuestring : "",
			    cJSON_IsString(jname) ? jname->valuestring : "", NULL);
		}
		return NULL;
	}

	if (strcmp(type, "content_block_delta") == 0) {
		cJSON *jindex = cJSON_GetObjectItemCaseSensitive(raw, "index");
		cJSON *delta = cJSON_GetObjectItemCaseSensitive(raw, "delta");
		cJSON *jdtype = delta ? cJSON_GetObjectItemCaseSensitive(delta, "type") : NULL;
		const char *dtype = cJSON_IsString(jdtype) ? jdtype->valuestring : "";
		int index = cJSON_IsNumber(jindex) ? (int)jindex->valuedouble : 0;

		if (strcmp(dtype, "text_delta") == 0) {
			cJSON *jtext = cJSON_GetObjectItemCaseSensitive(delta, "text");
			return make_delta_chunk("content", cJSON_IsString(jtext) ? jtext->valuestring : "");
		}
		if (strcmp(dtype, "thinking_delta") == 0) {
			cJSON *jthink = cJSON_GetObjectItemCaseSensitive(delta, "thinking");
			return make_delta_chunk("reasoning_content",
			    cJSON_IsString(jthink) ? jthink->valuestring : "");
		}
		if (strcmp(dtype, "input_json_delta") == 0) {
			cJSON *jpart = cJSON_GetObjectItemCaseSensitive(delta, "partial_json");
			return make_tool_delta_chunk(index, NULL, NULL,
			    cJSON_IsString(jpart) ? jpart->valuestring : "");
		}
		return NULL;
	}

	if (strcmp(type, "message_start") == 0) {
		cJSON *message = cJSON_GetObjectItemCaseSensitive(raw, "message");
		cJSON *usage = message ? cJSON_GetObjectItemCaseSensitive(message, "usage") : NULL;
		cJSON *jin = usage ? cJSON_GetObjectItemCaseSensitive(usage, "input_tokens") : NULL;
		struct anth_stream_state *st = stream_state(state);

		if (st != NULL && cJSON_IsNumber(jin))
			st->input_tokens = jin->valuedouble;
		return NULL;
	}

	if (strcmp(type, "message_delta") == 0) {
		cJSON *delta = cJSON_GetObjectItemCaseSensitive(raw, "delta");
		cJSON *jstop = delta ? cJSON_GetObjectItemCaseSensitive(delta, "stop_reason") : NULL;
		cJSON *usage = cJSON_GetObjectItemCaseSensitive(raw, "usage");
		cJSON *jout = usage ? cJSON_GetObjectItemCaseSensitive(usage, "output_tokens") : NULL;
		struct anth_stream_state *st = stream_state(state);
		cJSON *out, *choices, *choice0;

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
		if (cJSON_IsString(jstop)) {
			const char *mapped = map_stop_reason(jstop->valuestring);
			if (mapped != NULL)
				cJSON_AddItemToObject(choice0, "finish_reason", cJSON_CreateString(mapped));
		}
		if (cJSON_IsNumber(jout)) {
			double itok = st != NULL ? st->input_tokens : 0;
			cJSON *cu = cJSON_CreateObject();
			if (cu != NULL) {
				cJSON_AddItemToObject(cu, "prompt_tokens", cJSON_CreateNumber(itok));
				cJSON_AddItemToObject(cu, "completion_tokens", cJSON_CreateNumber(jout->valuedouble));
				cJSON_AddItemToObject(cu, "total_tokens", cJSON_CreateNumber(itok + jout->valuedouble));
				cJSON_AddItemToObject(out, "usage", cu);
			}
		}
		return out;
	}

	/* message_stop, content_block_stop, ping, error: nothing to merge. */
	return NULL;
}

const struct clm_provider_ops clm_provider_ops_anthropic = {
	.build_request = anthropic_build_request,
	.build_auth_headers = anthropic_build_auth_headers,
	.normalize_response = anthropic_normalize_response,
	.normalize_stream_event = anthropic_normalize_stream_event,
};
