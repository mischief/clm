// SPDX-License-Identifier: ISC
/*
 * test_lua_plugin -- verify that Lua plugins load and tools get registered.
 * Only built at all when lib/'s lua feature is enabled (test/meson.build);
 * see the clmlua split in lib/meson.build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
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

	struct clm_host *host = NULL;
	r = clm_host_uv_new(loop, &host);
	CHECK(r == 0, "host creation");
	r = clm_agent_new(&cfg, host, NULL, NULL, &agent);
	CHECK(r == 0, "agent creation");
	if (r != 0)
		return 1;

	r = clm_lua_env_new(agent, &env);
	CHECK(r == 0, "lua env creation");

	r = clm_lua_load_plugins(env, "plugins");
	CHECK(r == 0, "plugin loading");

	/* Verify the reverse_string tool was registered. */
	int found = 0;
	struct clm_tool *rt;
	TAILQ_FOREACH(rt, &agent->tools, entries) {
		if (strcmp(rt->name, "reverse_string") == 0) {
			found = 1;
			CHECK(rt->description != NULL, "tool has description");
			CHECK(rt->params_schema != NULL, "tool has params_schema");
			CHECK(rt->invoke != NULL, "tool has invoke function");
			break;
		}
	}
	CHECK(found, "reverse_string tool found in registry");

	/* Verify the schema is valid JSON. */
	if (found) {
		TAILQ_FOREACH(rt, &agent->tools, entries) {
			if (strcmp(rt->name, "reverse_string") != 0)
				continue;
			struct json_object *schema =
			    json_tokener_parse(rt->params_schema);
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
	clm_host_uv_free(host);
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

	struct clm_host *host = NULL;
	r = clm_host_uv_new(loop, &host);
	CHECK(r == 0, "host creation");
	r = clm_agent_new(&cfg, host, NULL, NULL, &agent);
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
	clm_host_uv_free(host);
	return 0;
}

static int
tool_registered(struct clm_agent *agent, const char *name)
{
	struct clm_tool *t;
	TAILQ_FOREACH(t, &agent->tools, entries)
		if (strcmp(t->name, name) == 0)
			return 1;
	return 0;
}

/*
 * Load the self-test plugin dir, which contains one clean plugin plus three
 * that must fail to load gracefully: an explicit error(), an indirect nil
 * call, and an infinite loop at file scope (bounded by the load-time CPU cap).
 * The clean plugin's marker tool must be registered; reaching this point at
 * all proves the loop plugin did not hang the process.
 */
static int
test_sandbox_and_load_failures(void)
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

	struct clm_host *host = NULL;
	r = clm_host_uv_new(loop, &host);
	CHECK(r == 0, "host creation");
	r = clm_agent_new(&cfg, host, NULL, NULL, &agent);
	CHECK(r == 0, "agent creation (sandbox)");
	if (r != 0)
		return 1;

	r = clm_lua_env_new(agent, &env);
	CHECK(r == 0, "lua env creation (sandbox)");

	/* Returns 0 even though individual plugins fail; failures are skipped.
	 * Reaching the next line proves loop_at_load.lua did not hang. */
	r = clm_lua_load_plugins(env, "test/plugins");
	CHECK(r == 0, "self-test plugin dir loads (failures skipped, no hang)");

	/* The clean, sandbox-asserting plugin loaded and registered its marker:
	 * the sandbox denies os/io/require/load and exposes only safe libs. */
	CHECK(tool_registered(agent, "sandbox_ok_marker"),
	    "sandbox-clean plugin loaded (no unsafe globals reachable)");

	/* The failing plugins registered nothing (they raised before/instead of
	 * registering); they must not have leaked tools into the registry. */
	CHECK(!tool_registered(agent, "fail_explicit_marker"),
	    "explicit-error load failure registered no tool");
	CHECK(!tool_registered(agent, "fail_indirect_marker"),
	    "indirect-error load failure registered no tool");
	CHECK(!tool_registered(agent, "loop_marker"),
	    "infinite-loop load failure registered no tool");

	clm_lua_env_free(env);
	clm_agent_free(agent);
	clm_host_uv_free(host);
	return 0;
}

int
main(void)
{
	printf("test_lua_plugin: running...\n");
	test_plugin_loads();
	test_nonexistent_dir();
	test_sandbox_and_load_failures();

	if (failures > 0) {
		printf("test_lua_plugin: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_lua_plugin: PASS\n");
	return 0;
}
