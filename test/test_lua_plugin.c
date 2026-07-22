// SPDX-License-Identifier: ISC
/*
 * test_lua_plugin -- verify that Lua plugins load and tools get registered.
 * Only built at all when lib/'s lua feature is enabled (test/meson.build);
 * see the clmlua split in lib/meson.build.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
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

struct pending_host;

struct clm_http_call {
	struct pending_host *owner;
};

struct clm_timer {
	struct pending_host *owner;
};

struct pending_host {
	struct clm_http_call http_call;
	struct clm_timer timer;
	clm_http_success_cb http_success;
	clm_http_error_cb http_error;
	void *http_user;
	clm_timer_cb timer_cb;
	void *timer_arg;
	int http_cancelled;
	int timer_cancelled;
};

static int
pending_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	struct pending_host *pending = ctx;

	(void)req;
	(void)data;
	pending->http_call.owner = pending;
	pending->http_success = success;
	pending->http_error = error;
	pending->http_user = user;
	*out = &pending->http_call;
	return 0;
}

static void
pending_http_cancel(struct clm_http_call *call)
{
	call->owner->http_cancelled++;
}

static int
pending_timer_set(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
    struct clm_timer **out)
{
	struct pending_host *pending = ctx;

	(void)ms;
	pending->timer.owner = pending;
	pending->timer_cb = cb;
	pending->timer_arg = arg;
	*out = &pending->timer;
	return 0;
}

static void
pending_timer_cancel(struct clm_timer *timer)
{
	timer->owner->timer_cancelled++;
}

static int
start_pending_tool(struct clm_agent *agent, struct pending_host *pending,
    const char *name)
{
	clm_http_success_cb completion;
	struct clm_http_response response = {0};
	char json[512];
	void *completion_user;
	int r;

	r = clm_agent_submit(agent, "run the pending tool");
	if (r < 0 || pending->http_success == NULL)
		return r < 0 ? r : -EIO;
	completion = pending->http_success;
	completion_user = pending->http_user;
	(void)snprintf(json, sizeof(json),
	    "{\"choices\":[{\"message\":{\"role\":\"assistant\","
	    "\"content\":null,\"tool_calls\":[{\"id\":\"pending-1\","
	    "\"type\":\"function\",\"function\":{\"name\":\"%s\","
	    "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}",
	    name);
	response.status_code = 200;
	response.body = strdup(json);
	if (response.body == NULL)
		return -ENOMEM;
	completion(&response, completion_user);
	return 0;
}

static int
pending_setup(struct pending_host *pending, struct clm_host *host,
    struct clm_agent **agent, struct clm_lua_env **env)
{
	struct clm_cfg cfg = {
		.api_key = "test",
		.base_url = "http://pending.invalid/v1/chat/completions",
		.provider = CLM_PROVIDER_OPENAI,
		.model = "test",
		.max_iterations = 1,
		.stream = 0,
	};
	int r;

	memset(host, 0, sizeof(*host));
	host->http_post = pending_http_post;
	host->http_cancel = pending_http_cancel;
	host->timer_set = pending_timer_set;
	host->timer_cancel = pending_timer_cancel;
	host->ctx = pending;
	r = clm_agent_new(&cfg, host, NULL, NULL, agent);
	if (r < 0)
		return r;
	r = clm_lua_env_new(*agent, env);
	if (r < 0)
		return r;
	return clm_lua_load_plugins(*env, "test/plugins_teardown");
}

static int
test_pending_http_teardown(void)
{
	struct pending_host pending = {0};
	struct clm_host host;
	struct clm_agent *agent = NULL;
	struct clm_lua_env *env = NULL;
	clm_http_error_cb late_error;
	void *late_user;
	int r;

	r = pending_setup(&pending, &host, &agent, &env);
	CHECK(r == 0, "pending http setup");
	if (r < 0)
		return 1;
	r = start_pending_tool(agent, &pending, "pending_http");
	CHECK(r == 0, "pending http dispatch");
	CHECK(pending.http_error != NULL, "pending http request started");
	late_error = pending.http_error;
	late_user = pending.http_user;

	clm_lua_env_free(env);
	CHECK(pending.http_cancelled == 1, "pending http request cancelled");
	clm_agent_free(agent);
	if (late_error != NULL)
		late_error(-ECANCELED, "cancelled", late_user);
	return 0;
}

static int
test_pending_sleep_teardown(void)
{
	struct pending_host pending = {0};
	struct clm_host host;
	struct clm_agent *agent = NULL;
	struct clm_lua_env *env = NULL;
	int r;

	r = pending_setup(&pending, &host, &agent, &env);
	CHECK(r == 0, "pending sleep setup");
	if (r < 0)
		return 1;
	r = start_pending_tool(agent, &pending, "pending_sleep");
	CHECK(r == 0, "pending sleep dispatch");
	CHECK(pending.timer_cb != NULL, "pending sleep timer started");

	clm_lua_env_free(env);
	CHECK(pending.timer_cancelled == 1, "pending sleep timer cancelled");
	clm_agent_free(agent);
	return 0;
}

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
			cJSON *schema = cJSON_Parse(rt->params_schema);
			CHECK(schema != NULL, "params_schema is valid JSON");
			if (schema) {
				cJSON *props =
				    cJSON_GetObjectItemCaseSensitive(schema, "properties");
				CHECK(props != NULL, "schema has properties");
				if (props) {
					cJSON *text_prop =
					    cJSON_GetObjectItemCaseSensitive(props, "text");
					CHECK(text_prop != NULL,
					    "schema has 'text' property");
				}
				cJSON_Delete(schema);
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

struct inline_http_host {
	struct clm_host host;
	struct clm_host *uv_host;
	int calls;
};

struct inline_http_state {
	int done;
	int status;
	int results;
	char content[256];
};

static int
inline_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	struct inline_http_host *host = ctx;

	if (out != NULL)
		*out = NULL;
	if (strcmp(req->url, "test://inline/success") == 0) {
		struct clm_http_response resp = {
			.status_code = 200,
			.body = strdup("inline body"),
		};
		host->calls++;
		if (resp.body == NULL)
			error((int)CURLE_OUT_OF_MEMORY, "curl error: out of memory", user);
		else
			success(&resp, user);
		return 0;
	}
	if (strcmp(req->url, "test://inline/connect-error") == 0) {
		host->calls++;
		error((int)CURLE_COULDNT_CONNECT, "curl error: could not connect", user);
		return 0;
	}
	return host->uv_host->http_post(host->uv_host->ctx, req, success, error,
	    data, user, out);
}

static int
inline_timer_set(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
    struct clm_timer **out)
{
	struct inline_http_host *host = ctx;

	return host->uv_host->timer_set(host->uv_host->ctx, ms, cb, arg, out);
}

static void
inline_on_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	struct inline_http_state *state = user;

	(void)name;
	(void)outcome;
	state->results++;
	(void)snprintf(state->content, sizeof(state->content), "%s",
	    content != NULL ? content : "");
}

static void
inline_on_turn_done(int status, void *user)
{
	struct inline_http_state *state = user;

	state->done = 1;
	state->status = status;
}

static int
test_inline_http_completion(void)
{
	uv_loop_t loop;
	struct canned_server *srv;
	struct inline_http_host host = {0};
	struct inline_http_state state = {0};
	struct clm_agent *agent = NULL;
	struct clm_lua_env *env = NULL;
	struct clm_callbacks callbacks = {
		.on_tool_result = inline_on_tool_result,
		.on_turn_done = inline_on_turn_done,
	};
	struct clm_cfg cfg = {
		.api_key = "test",
		.provider = CLM_PROVIDER_OPENAI,
		.model = "test",
		.max_iterations = 2,
	};
	char url[128];
	int r;

	CHECK(uv_loop_init(&loop) == 0, "inline loop init");
	srv = canned_start(&loop);
	CHECK(srv != NULL, "inline canned server");
	if (srv == NULL)
		return 1;
	(void)snprintf(url, sizeof(url),
	    "http://127.0.0.1:%d/v1/chat/completions", canned_port(srv));
	cfg.base_url = url;

	r = clm_host_uv_new(&loop, &host.uv_host);
	CHECK(r == 0, "inline uv host creation");
	host.host.http_post = inline_http_post;
	host.host.http_cancel = host.uv_host->http_cancel;
	host.host.timer_set = inline_timer_set;
	host.host.timer_cancel = host.uv_host->timer_cancel;
	host.host.ctx = &host;

	r = clm_agent_new(&cfg, &host.host, &callbacks, &state, &agent);
	CHECK(r == 0, "inline agent creation");
	r = clm_lua_env_new(agent, &env);
	CHECK(r == 0, "inline lua env creation");
	r = clm_lua_load_plugins(env, "test/plugins");
	CHECK(r == 0, "inline plugin loading");

	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"tool_calls\","
	    "\"message\":{\"role\":\"assistant\",\"content\":\"\","
	    "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
	    "\"function\":{\"name\":\"http_inline\","
	    "\"arguments\":\"{}\"}}]}}]}");
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\","
	    "\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}");

	CHECK(clm_agent_submit(agent, "run inline http") == 0, "inline submit");
	while (!state.done)
		uv_run(&loop, UV_RUN_ONCE);
	CHECK(state.status == 0, "inline turn status");
	CHECK(state.results == 1, "inline tool result count");
	CHECK(strcmp(state.content,
	    "inline body:curl error: could not connect") == 0,
	    "inline tool result content");
	CHECK(host.calls == 2, "inline host call count");

	clm_lua_env_free(env);
	clm_agent_free(agent);
	clm_host_uv_free(host.uv_host);
	canned_stop(srv);
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(uv_loop_close(&loop) == 0, "inline loop close");
	return 0;
}

int
main(void)
{
	printf("test_lua_plugin: running...\n");
	test_plugin_loads();
	test_nonexistent_dir();
	test_sandbox_and_load_failures();
	test_pending_http_teardown();
	test_pending_sleep_teardown();
	test_inline_http_completion();

	if (failures > 0) {
		printf("test_lua_plugin: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_lua_plugin: PASS\n");
	return 0;
}
