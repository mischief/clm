// SPDX-License-Identifier: ISC
/*
 * test_lua_agent -- exercise the agent.new()/:submit()/:free() subagent
 * prototype (lua_agent.c) against a canned HTTP server, on the same uv loop
 * as the parent agent (mirrors how a real tool invocation runs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
#include "clm/internal.h"
#include "clm/lua_plugin.h"
#include "canned.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                    \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const char *final_reply =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
    "\"message\":{\"role\":\"assistant\",\"content\":\"hi from child\"}}]}";

struct tstate {
	uv_loop_t *loop;
	struct clm_host *host;
	struct clm_agent *agent;
	struct clm_lua_env *env;
	struct canned_server *srv;
	int turn_done;
	int turn_status;
	char assistant[256];
};

static void
on_assistant_text(const char *text, void *user)
{
	struct tstate *st = user;
	(void)snprintf(st->assistant, sizeof(st->assistant), "%s", text ? text : "");
}

static void
on_turn_done(int status, void *user)
{
	struct tstate *st = user;
	st->turn_done = 1;
	st->turn_status = status;
}

static const struct clm_callbacks parent_callbacks = {
	.on_assistant_text = on_assistant_text,
	.on_turn_done = on_turn_done,
};

static int
test_spawn_and_submit(void)
{
	struct tstate st = {0};
	char base_url[256];
	int r;

	st.loop = uv_default_loop();
	r = clm_host_uv_new(st.loop, &st.host);
	CHECK(r == 0, "host creation");

	st.srv = canned_start(st.loop);
	CHECK(st.srv != NULL, "canned server start");
	if (st.srv == NULL)
		return 1;

	(void)snprintf(base_url, sizeof(base_url),
	    "http://127.0.0.1:%d/v1/chat/completions", canned_port(st.srv));

	struct clm_cfg cfg = {
		.api_key = "test",
		.base_url = base_url,
		.provider = CLM_PROVIDER_OPENAI,
		.model = "test",
		.max_iterations = 4,
		.stream = 0,
	};

	r = clm_agent_new(&cfg, st.host, &parent_callbacks, &st, &st.agent);
	CHECK(r == 0, "parent agent creation");
	if (r != 0)
		return 1;

	r = clm_lua_env_new(st.agent, &st.env);
	CHECK(r == 0, "lua env creation");

	r = clm_lua_load_plugins(st.env, "test/plugins_agent");
	CHECK(r == 0, "plugin dir loads");

	/* Find and invoke spawn_test directly via the tool registry, exactly
	 * as clm_tools_dispatch would, but without needing a canned parent
	 * LLM response requesting it. */
	struct clm_tool *t;
	struct clm_tool *found = NULL;
	TAILQ_FOREACH(t, &st.agent->tools, entries) {
		if (strcmp(t->name, "spawn_test") == 0) {
			found = t;
			break;
		}
	}
	CHECK(found != NULL, "spawn_test tool registered");
	if (found == NULL)
		return 1;

	/* Drive it through a real tool-call turn so clm_tools_dispatch's
	 * async machinery (and thus the coroutine yield/resume path in
	 * lua_agent.c) is exercised faithfully. Queue a parent response that
	 * calls spawn_test, then a second queued child reply already set
	 * above, then a parent follow-up reply once the tool result comes
	 * back. */
	{
		cJSON *root = cJSON_CreateObject();
		cJSON *choices = cJSON_CreateArray();
		cJSON *choice = cJSON_CreateObject();
		cJSON *message = cJSON_CreateObject();
		cJSON *calls = cJSON_CreateArray();
		cJSON *call = cJSON_CreateObject();
		cJSON *func = cJSON_CreateObject();
		char *printed;

		cJSON_AddItemToObject(func, "name", cJSON_CreateString("spawn_test"));
		cJSON_AddItemToObject(func, "arguments", cJSON_CreateString("{}"));
		cJSON_AddItemToObject(call, "id", cJSON_CreateString("c1"));
		cJSON_AddItemToObject(call, "type", cJSON_CreateString("function"));
		cJSON_AddItemToObject(call, "function", func);
		cJSON_AddItemToArray(calls, call);
		cJSON_AddItemToObject(message, "role", cJSON_CreateString("assistant"));
		cJSON_AddItemToObject(message, "content", cJSON_CreateString(""));
		cJSON_AddItemToObject(message, "tool_calls", calls);
		cJSON_AddItemToObject(choice, "finish_reason", cJSON_CreateString("tool_calls"));
		cJSON_AddItemToObject(choice, "message", message);
		cJSON_AddItemToArray(choices, choice);
		cJSON_AddItemToObject(root, "choices", choices);
		printed = cJSON_PrintUnformatted(root);
		canned_reply(st.srv, printed != NULL ? printed : "{}");
		free(printed);
		cJSON_Delete(root);
	}
	/* FIFO order: (1) parent's tool-call response above, (2) the child
	 * subagent's own turn (final_reply), (3) parent's follow-up once the
	 * tool result comes back. */
	canned_reply(st.srv, final_reply);
	canned_reply(st.srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"parent done\"}}]}");

	r = clm_agent_submit(st.agent, "go");
	CHECK(r == 0, "parent submit accepted");

	for (int i = 0; i < 2000 && !st.turn_done; i++)
		uv_run(st.loop, UV_RUN_ONCE);

	CHECK(st.turn_done, "parent turn completed");
	CHECK(st.turn_status == 0, "parent turn succeeded");
	CHECK(strstr(st.assistant, "parent done") != NULL,
	    "parent's final assistant text observed");

	clm_lua_env_free(st.env);
	clm_agent_free(st.agent);
	canned_stop(st.srv);
	uv_run(st.loop, UV_RUN_ONCE);
	clm_host_uv_free(st.host);
	return 0;
}

int
main(void)
{
	printf("test_lua_agent: running...\n");
	test_spawn_and_submit();

	if (failures > 0) {
		printf("test_lua_agent: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_lua_agent: PASS\n");
	return 0;
}
