// SPDX-License-Identifier: ISC
/*
 * test_lua_plugin -- verify that Lua plugins load and tools get registered.
 */
#ifdef CLM_LUA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/lua_plugin.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                    \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static int
test_plugin_loads(void)
{
	struct clm_agent *agent = NULL;
	struct clm_lua_env *env = NULL;
	uv_loop_t *loop = uv_default_loop();
	struct clm_cfg cfg = {
		.api_key = "test",
		.base_url = "http://127.0.0.1:1/v1/chat/completions",
		.provider = CLM_PROVIDER_OPENAI,
		.model = "test",
		.max_iterations = 1,
		.stream = 0,
	};
	int r;

	r = clm_agent_new(&cfg, loop, NULL, NULL, &agent);
	CHECK(r == 0, "agent creation");
	if (r != 0)
		return 1;

	r = clm_lua_env_new(agent, &env);
	CHECK(r == 0, "lua env creation");

	r = clm_lua_load_plugins(env, "plugins");
	CHECK(r == 0, "plugin loading");

	/* Verify the reverse_string tool was registered. */
	int found = 0;
	for (size_t i = 0; i < agent->tool_count; i++) {
		if (strcmp(agent->tools[i].name, "reverse_string") == 0) {
			found = 1;
			CHECK(agent->tools[i].description != NULL,
			    "tool has description");
			CHECK(agent->tools[i].params_schema != NULL,
			    "tool has params_schema");
			CHECK(agent->tools[i].invoke != NULL,
			    "tool has invoke function");
			break;
		}
	}
	CHECK(found, "reverse_string tool found in registry");

	/* Verify the schema is valid JSON. */
	if (found) {
		for (size_t i = 0; i < agent->tool_count; i++) {
			if (strcmp(agent->tools[i].name, "reverse_string") != 0)
				continue;
			struct json_object *schema =
			    json_tokener_parse(agent->tools[i].params_schema);
			CHECK(schema != NULL, "params_schema is valid JSON");
			if (schema) {
				struct json_object *props = NULL;
				json_object_object_get_ex(schema, "properties",
				    &props);
				CHECK(props != NULL, "schema has properties");
				if (props) {
					struct json_object *text_prop = NULL;
					json_object_object_get_ex(props, "text",
					    &text_prop);
					CHECK(text_prop != NULL,
					    "schema has 'text' property");
				}
				json_object_put(schema);
			}
		}
	}

	clm_lua_env_free(env);
	clm_agent_free(agent);
	return 0;
}

static int
test_nonexistent_dir(void)
{
	struct clm_agent *agent = NULL;
	struct clm_lua_env *env = NULL;
	uv_loop_t *loop = uv_default_loop();
	struct clm_cfg cfg = {
		.api_key = "test",
		.base_url = "http://127.0.0.1:1/v1/chat/completions",
		.provider = CLM_PROVIDER_OPENAI,
		.model = "test",
		.max_iterations = 1,
		.stream = 0,
	};
	int r;

	r = clm_agent_new(&cfg, loop, NULL, NULL, &agent);
	CHECK(r == 0, "agent creation (nonexistent)");
	if (r != 0)
		return 1;

	r = clm_lua_env_new(agent, &env);
	CHECK(r == 0, "lua env creation (nonexistent)");

	/* Should succeed gracefully when directory doesn't exist. */
	r = clm_lua_load_plugins(env, "/nonexistent/path/to/plugins");
	CHECK(r == 0, "nonexistent dir returns 0");

	clm_lua_env_free(env);
	clm_agent_free(agent);
	return 0;
}

int
main(void)
{
	printf("test_lua_plugin: running...\n");
	test_plugin_loads();
	test_nonexistent_dir();

	if (failures > 0) {
		printf("test_lua_plugin: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_lua_plugin: PASS\n");
	return 0;
}

#else /* !CLM_LUA */

#include <stdio.h>

int
main(void)
{
	printf("test_lua_plugin: SKIPPED (CLM_LUA not enabled)\n");
	return 0;
}

#endif
