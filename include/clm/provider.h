// SPDX-License-Identifier: ISC
/*
 * Per-provider wire-format ops -- not installed, library-internal.
 *
 * clm_provider (clm/clm.h) is metadata about which wire dialect a backend
 * speaks; this is the vtable that acts on it. clm's request building
 * (clm_agent_start_turn) and response parsing (the non-streaming success
 * callback and the SSE line handler, both in agent.c) work exclusively in
 * one canonical, OpenAI chat-completions-shaped representation:
 *
 *   - request:            {model, messages, stream, tools, ...}
 *   - non-streaming reply: choices[0].message.{content, tool_calls}
 *   - streaming reply:     choices[0].delta.{content, tool_calls} per chunk
 *
 * A provider whose wire format already matches that shape (OpenAI itself,
 * and every OpenAI-compatible local server -- which is why
 * CLM_PROVIDER_OLLAMA also uses these ops) needs no translation at all; see
 * provider_openai.c. A provider with a structurally different wire format
 * (Anthropic's Messages API: a single Message with a content-block array
 * instead of choices, {id, name, input} tool_use blocks, a separate system
 * field, a content_block_delta/message_delta event stream) instead
 * translates at this one seam: build_request emits that provider's own
 * request shape from the canonical messages/tools, and
 * normalize_response/normalize_stream_event translate its response/event
 * shapes back into the canonical one before any other code in agent.c ever
 * sees them. See provider_anthropic.c.
 *
 * This keeps every other file (tools.c's tool-call parsing, history.c's
 * message serialization, and the bulk of agent.c) unaware that more than
 * one wire format exists.
 */
#ifndef CLM_PROVIDER_H
#define CLM_PROVIDER_H

#include <stdbool.h>

#include <cJSON.h>

#include "clm/clm.h"
#include "clm/llm.h"

struct clm_provider_ops {
	/*
	 * Build the outgoing request body from the canonical messages/tools
	 * arrays clm_agent_start_turn() already assembled (clm_history_to_json/
	 * clm_tools_build_schema output). Always takes ownership of both --
	 * every implementation either incorporates or deletes them, even on
	 * failure. Returns a new request object the caller owns, or NULL on
	 * OOM. Never NULL in the vtable itself.
	 */
	cJSON *(*build_request)(const struct clm_llm *llm, cJSON *messages,
	    cJSON *tools, bool stream);

	/*
	 * Extra HTTP headers this provider's auth scheme needs in place of the
	 * default bearer-token header -- e.g. Anthropic's
	 * x-api-key/anthropic-version. Returns a NULL-terminated array of
	 * malloc'd "Name: Value" strings (the caller frees each string and the
	 * array), or NULL to mean "use the default bearer-token header, no
	 * extra headers needed". May be NULL in the vtable itself, same
	 * meaning.
	 */
	char **(*build_auth_headers)(const struct clm_llm *llm);

	/*
	 * Translate one full non-streaming response body (already parsed) into
	 * the canonical choices[0].message shape. Returns a new tree the
	 * caller owns (to replace raw) or NULL on OOM/malformed input. NULL in
	 * the vtable itself (not the return value) means "raw is already
	 * canonical, no translation needed" -- see provider_openai.c.
	 */
	cJSON *(*normalize_response)(cJSON *raw);

	/*
	 * Translate one parsed SSE "data:" payload into the canonical
	 * choices[0].delta chunk shape stream_handle_line() reads, or return
	 * NULL to mean "nothing to merge from this event" (e.g. Anthropic's
	 * content_block_stop/message_stop bookkeeping events carry no delta).
	 * `raw` is borrowed (unlike build_request/normalize_response, this does
	 * NOT take ownership -- the caller parses and frees each SSE line's
	 * JSON itself); the returned chunk, if any, is a new tree the caller
	 * owns. *state is a per-turn scratch slot (initially NULL, plain
	 * free()'d when the turn ends) a provider may use to carry information
	 * between events of the same stream -- e.g. Anthropic's message_start
	 * carries input token usage that message_delta's own event doesn't
	 * repeat, so the translator stashes it here until the completion count
	 * arrives. NULL in the vtable itself means "raw is already canonical".
	 */
	cJSON *(*normalize_stream_event)(cJSON *raw, void **state);
};

/* Look up the ops for a provider. Never returns NULL: any provider without
 * a wire format of its own (currently OPENAI and OLLAMA) gets the identity
 * ops in provider_openai.c. */
const struct clm_provider_ops *clm_provider_ops_get(enum clm_provider provider);

#endif /* CLM_PROVIDER_H */
