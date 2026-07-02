// SPDX-License-Identifier: ISC
/*
 * Fuzz target: clm_tools_dispatch()
 *
 * Feeds arbitrary bytes as a JSON tool-calls array into the dispatch
 * engine. Tests how clm parses, validates, and handles adversarial
 * model responses with malformed or malicious tool calls.
 *
 * Build: CC=clang ninja -C build-fuzz fuzz/fuzz_tool_dispatch
 * Run:   ./build-fuzz/fuzz/fuzz_tool_dispatch
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/cleanup.h"

/* libFuzzer entry; declared here so -Wmissing-prototypes is satisfied. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* ------------------------------------------------------------------ */
/* Minimal stub host (no real I/O)                                     */
/* ------------------------------------------------------------------ */

static int
stub_http_post(void *ctx, const struct clm_http_req *req,
               clm_http_success_cb success, clm_http_error_cb error,
               clm_http_data_cb data, void *user,
               struct clm_http_call **out)
{
	(void)ctx; (void)req; (void)success; (void)error;
	(void)data; (void)user; (void)out;
	return -ENOSYS;
}

static int
stub_timer_set(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
               struct clm_timer **out)
{
	(void)ctx; (void)ms; (void)cb; (void)arg;
	*out = NULL; /* no timer; dispatch runs immediately */
	return 0;
}

static void stub_timer_cancel(struct clm_timer *t) { (void)t; }
static void stub_http_cancel(struct clm_http_call *c) { (void)c; }

static struct clm_host stub_host = {
	.http_post    = stub_http_post,
	.http_cancel  = stub_http_cancel,
	.timer_set    = stub_timer_set,
	.timer_cancel = stub_timer_cancel,
};

/* ------------------------------------------------------------------ */
/* Dummy tool: completes synchronously with fixed output               */
/* ------------------------------------------------------------------ */

static void
noop_invoke(struct clm_tool_invocation *inv, void *user)
{
	(void)user;
	clm_tool_complete(inv, "ok");
}

static void
register_tool(struct clm_agent *agent, const char *name)
{
	struct clm_tool_def def = {
		.name          = name,
		.description   = "dummy",
		.params_schema = "{}",
		.invoke        = noop_invoke,
		.output_cap    = 4096,
		.timeout_ms    = 5000,
		.flags         = CLM_TOOL_NO_PROMPT, /* skip permission gate */
	};
	clm_tool_add(agent, &def);
}

/* ------------------------------------------------------------------ */

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct clm_agent *agent;
	struct clm_cfg cfg;
	json_cleanup struct json_object *root = NULL;
	struct json_object *tool_calls = NULL;

	if (size == 0)
		return 0;

	/* Parse fuzz input as JSON.  Invalid input returns NULL, which is safe. */
	struct json_tokener *tok = json_tokener_new();
	if (tok == NULL)
		return 0;
	root = json_tokener_parse_ex(tok, (const char *)data, (int)size);
	json_tokener_free(tok);
	if (root == NULL)
		return 0;

	/* Extract a JSON array (either top-level or nested under common keys). */
	if (json_object_get_type(root) == json_type_array) {
		tool_calls = root;
	} else if (json_object_get_type(root) == json_type_object) {
		struct json_object *tc;
		if (json_object_object_get_ex(root, "tool_calls", &tc) ||
		    json_object_object_get_ex(root, "choices", &tc) ||
		    json_object_object_get_ex(root, "arguments", &tc)) {
			if (json_object_get_type(tc) == json_type_array)
				tool_calls = tc;
		}
	}

	if (tool_calls == NULL || json_object_array_length(tool_calls) == 0)
		return 0;

	/* Build a fresh agent per input. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.api_key       = "fuzz";
	cfg.base_url      = "http://localhost/v1/chat/completions";
	cfg.provider      = CLM_PROVIDER_OPENAI;
	cfg.model         = "fuzz-model";
	cfg.system_prompt = "fuzz";

	if (clm_agent_new(&cfg, &stub_host, NULL, NULL, &agent) < 0)
		return 0;

	/* Register 512 dummy tools to stress the registry lookup. */
	for (int i = 0; i < 512; i++) {
		char name[32];
		if (snprintf(name, sizeof(name), "tool_%d", i) > 0)
			register_tool(agent, name);
	}

	/* Dispatch the adversarial tool-calls array. */
	clm_tools_dispatch(agent, tool_calls);

	clm_agent_free(agent);
	return 0;
}
