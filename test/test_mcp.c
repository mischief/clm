// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
#include "clm/mcp.h"
#include "canned.h"

static int failures;

#define check(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "fail: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                         \
			failures++;                                            \
		}                                                              \
	} while (0)

struct state {
	uv_loop_t *loop;
	struct clm_host *host;
	struct clm_agent *agent;
	bool ready;
	int ready_status;
	size_t ready_tools;
	bool turn_done;
	int turn_status;
	int tool_results;
	enum clm_tool_outcome outcome;
	char content[160];
	struct canned_server *resume_server;
};

static const char *mcp_init_response =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
    "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"serverInfo\":{\"name\":\"test\",\"version\":\"1\"}}}";

static const char *mcp_list_response =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{"
    "\"name\":\"slow\",\"description\":\"slow tool\","
    "\"inputSchema\":{\"type\":\"object\"}}]}}";

static const char *mcp_call_response =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{"
    "\"type\":\"text\",\"text\":\"late response\"}]}}";

static const char *final_response =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
    "\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}";

static void
on_ready(int status, size_t tool_count, void *user)
{
	struct state *st = user;
	st->ready = true;
	st->ready_status = status;
	st->ready_tools = tool_count;
}

static void
on_permission(const struct clm_permission_req *req, void *user)
{
	struct state *st = user;
	clm_tool_permission_respond(st->agent, req, CLM_PERM_ALLOW_ONCE);
}

static void
on_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	struct state *st = user;
	(void)name;
	st->tool_results++;
	st->outcome = outcome;
	(void)snprintf(st->content, sizeof(st->content), "%s",
	    content != NULL ? content : "");
	if (st->resume_server != NULL) {
		canned_resume(st->resume_server);
		st->resume_server = NULL;
	}
}

static void
on_turn_done(int status, void *user)
{
	struct state *st = user;
	st->turn_done = true;
	st->turn_status = status;
}

static const struct clm_callbacks callbacks = {
	.on_permission = on_permission,
	.on_tool_result = on_tool_result,
	.on_turn_done = on_turn_done,
};

static int
make_agent(struct state *st, struct canned_server *model)
{
	struct clm_cfg cfg = {0};
	char url[128];
	int r;

	(void)snprintf(url, sizeof(url),
	    "http://127.0.0.1:%d/v1/chat/completions", canned_port(model));
	cfg.api_key = "test";
	cfg.base_url = url;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = "test-model";

	r = clm_host_uv_new(st->loop, &st->host);
	if (r != 0)
		return r;
	r = clm_agent_new(&cfg, st->host, &callbacks, st, &st->agent);
	if (r != 0) {
		clm_host_uv_free(st->host);
		st->host = NULL;
	}
	return r;
}

static void
queue_tool_turn(struct canned_server *model, const char *tool_name)
{
	char response[768];

	(void)snprintf(response, sizeof(response),
	    "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"\","
	    "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
	    "\"function\":{\"name\":\"%s\",\"arguments\":\"{}\"}}]}}]}",
	    tool_name);
	canned_reply(model, response);
	canned_reply(model, final_response);
}

static void
run_until(uv_loop_t *loop, const bool *done)
{
	while (!*done)
		uv_run(loop, UV_RUN_ONCE);
}

struct wait_state {
	uv_timer_t timer;
	bool closed;
};

static void
wait_closed(uv_handle_t *handle)
{
	struct wait_state *wait = handle->data;
	wait->closed = true;
}

static void
wait_expired(uv_timer_t *timer)
{
	uv_close((uv_handle_t *)timer, wait_closed);
}

static void
run_for(uv_loop_t *loop, uint64_t timeout_ms)
{
	struct wait_state wait = {0};

	uv_timer_init(loop, &wait.timer);
	wait.timer.data = &wait;
	uv_timer_start(&wait.timer, wait_expired, timeout_ms, 0);
	while (!wait.closed)
		uv_run(loop, UV_RUN_ONCE);
}

static void
cleanup(struct state *st, struct clm_mcp_client *client,
    struct canned_server *model, struct canned_server *mcp)
{
	clm_mcp_client_free(client);
	clm_agent_free(st->agent);
	clm_host_uv_free(st->host);
	if (mcp != NULL)
		canned_stop(mcp);
	canned_stop(model);
	uv_run(st->loop, UV_RUN_DEFAULT);
}

static void
test_http_timeout(uv_loop_t *loop)
{
	struct state st = {.loop = loop};
	struct canned_server *model = canned_start(loop);
	struct canned_server *mcp = canned_start(loop);
	struct clm_mcp_client *client = NULL;
	struct clm_mcp_server_cfg cfg = {0};
	char url[128];
	int r;

	check(model != NULL && mcp != NULL, "http: start servers");
	if (model == NULL || mcp == NULL)
		return;
	check(make_agent(&st, model) == 0, "http: make agent");
	if (st.agent == NULL) {
		canned_stop(mcp);
		canned_stop(model);
		uv_run(loop, UV_RUN_DEFAULT);
		return;
	}

	canned_reply(mcp, mcp_init_response);
	canned_reply(mcp, mcp_list_response);
	canned_reply(mcp, mcp_call_response);
	(void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp",
	    canned_port(mcp));
	cfg.name = "http";
	cfg.transport = CLM_MCP_HTTP;
	cfg.url = url;
	cfg.timeout_ms = 10;

	r = clm_mcp_connect(st.agent, loop, &cfg, on_ready, &st, &client);
	check(r == 0, "http: connect");
	if (r != 0) {
		cleanup(&st, client, model, mcp);
		return;
	}
	run_until(loop, &st.ready);
	check(st.ready_status == 0 && st.ready_tools == 1, "http: ready");

	canned_pause_next(mcp);
	st.resume_server = mcp;
	queue_tool_turn(model, "mcp__http__slow");
	check(clm_agent_submit(st.agent, "call it") == 0, "http: submit");
	run_until(loop, &st.turn_done);
	run_for(loop, 25);

	check(st.turn_status == 0, "http: turn completed");
	check(st.tool_results == 1, "http: one tool result");
	check(st.outcome == CLM_TOOL_TIMEDOUT, "http: timed out");
	check(strstr(st.content, "timed out") != NULL, "http: timeout text");
	check(canned_request_count(mcp) == 3, "http: call reached server");
	cleanup(&st, client, model, mcp);
}

static void
test_stdio_timeout(uv_loop_t *loop)
{
	static const char script[] =
	    "while IFS= read -r line; do "
	    "case \"$line\" in "
	    "*'\"method\":\"initialize\"'*) "
	    "printf '%s\\n' '"
	    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}';; "
	    "*'\"method\":\"tools/list\"'*) "
	    "printf '%s\\n' '"
	    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{"
	    "\"name\":\"slow\",\"description\":\"slow tool\","
	    "\"inputSchema\":{\"type\":\"object\"}}]}}';; "
	    "*'\"method\":\"tools/call\"'*) sleep 0.05; "
	    "printf '%s\\n' '"
	    "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{"
	    "\"type\":\"text\",\"text\":\"late response\"}]}}';; "
	    "esac; done";
	struct state st = {.loop = loop};
	struct canned_server *model = canned_start(loop);
	struct clm_mcp_client *client = NULL;
	struct clm_mcp_server_cfg cfg = {0};
	char *argv[] = {"/bin/sh", "-c", (char *)script, NULL};
	int r;

	check(model != NULL, "stdio: start model server");
	if (model == NULL)
		return;
	check(make_agent(&st, model) == 0, "stdio: make agent");
	if (st.agent == NULL) {
		canned_stop(model);
		uv_run(loop, UV_RUN_DEFAULT);
		return;
	}

	cfg.name = "stdio";
	cfg.transport = CLM_MCP_STDIO;
	cfg.argv = argv;
	cfg.timeout_ms = 10;
	r = clm_mcp_connect(st.agent, loop, &cfg, on_ready, &st, &client);
	check(r == 0, "stdio: connect");
	if (r != 0) {
		cleanup(&st, client, model, NULL);
		return;
	}
	run_until(loop, &st.ready);
	check(st.ready_status == 0 && st.ready_tools == 1, "stdio: ready");

	queue_tool_turn(model, "mcp__stdio__slow");
	check(clm_agent_submit(st.agent, "call it") == 0, "stdio: submit");
	run_until(loop, &st.turn_done);
	run_for(loop, 100);

	check(st.turn_status == 0, "stdio: turn completed");
	check(st.tool_results == 1, "stdio: one tool result");
	check(st.outcome == CLM_TOOL_TIMEDOUT, "stdio: timed out");
	check(strstr(st.content, "timed out") != NULL, "stdio: timeout text");
	cleanup(&st, client, model, NULL);
}

int
main(void)
{
	uv_loop_t loop;

	uv_loop_init(&loop);
	test_http_timeout(&loop);
	test_stdio_timeout(&loop);
	check(uv_loop_close(&loop) == 0, "close loop");

	if (failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all mcp tests passed\n");
	return 0;
}
