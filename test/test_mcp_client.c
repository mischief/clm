// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/http_async.h"
#include "clm/mcp.h"
#include "clm/tools.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "fail: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                         \
			failures++;                                            \
		}                                                              \
	} while (0)

struct clm_http_request {
	clm_http_success_cb success;
	clm_http_error_cb error;
	void *user;
	bool cancelled;
};

struct test_state {
	struct clm_agent *agent;
	int ready_calls;
	int ready_status;
	size_t tool_count;
	int tool_results;
	enum clm_tool_outcome tool_outcome;
};

#define MOCK_REQUEST_MAX 4
static struct clm_http_request *mock_requests[MOCK_REQUEST_MAX];
static size_t mock_request_count;
static size_t mock_cancel_count;

int
clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
    const char *json_body, struct curl_slist *extra_headers,
    clm_http_success_cb success_cb, clm_http_error_cb error_cb,
    clm_http_data_cb data_cb, const char *client_suffix, void *user,
    struct clm_http_request **out_req)
{
	struct clm_http_request *req;

	(void)loop;
	(void)url;
	(void)api_key;
	(void)json_body;
	(void)extra_headers;
	(void)data_cb;
	(void)client_suffix;
	if (mock_request_count >= MOCK_REQUEST_MAX)
		return -E2BIG;
	req = calloc(1, sizeof(*req));
	if (req == NULL)
		return -ENOMEM;
	req->success = success_cb;
	req->error = error_cb;
	req->user = user;
	mock_requests[mock_request_count++] = req;
	if (out_req != NULL)
		*out_req = req;
	return 0;
}

void
clm_http_async_cancel(struct clm_http_request *req)
{
	if (req == NULL || req->cancelled)
		return;
	req->cancelled = true;
	mock_cancel_count++;
}

static void
mock_reset(void)
{
	memset(mock_requests, 0, sizeof(mock_requests));
	mock_request_count = 0;
	mock_cancel_count = 0;
}

static void
mock_success(struct clm_http_request *req, const char *body)
{
	struct clm_http_response resp = {
		.status_code = 200,
		.body = strdup(body),
	};

	CHECK(resp.body != NULL, "copy mock response");
	req->success(&resp, req->user);
	free(req);
}

static void
mock_error(struct clm_http_request *req)
{
	CHECK(req->cancelled, "request cancelled before settlement");
	req->error(-ECANCELED, "cancelled", req->user);
	free(req);
}

static int
host_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	(void)ctx;
	(void)req;
	(void)success;
	(void)data;
	if (out != NULL)
		*out = NULL;
	error(-EIO, "unavailable", user);
	return 0;
}

static void
host_http_cancel(struct clm_http_call *call)
{
	(void)call;
}

static int
host_timer_set(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
    struct clm_timer **out)
{
	(void)ctx;
	(void)ms;
	(void)cb;
	(void)arg;
	*out = NULL;
	return 0;
}

static void
host_timer_cancel(struct clm_timer *timer)
{
	(void)timer;
}

static struct clm_host host = {
	.http_post = host_http_post,
	.http_cancel = host_http_cancel,
	.timer_set = host_timer_set,
	.timer_cancel = host_timer_cancel,
};

static void
on_ready(int status, size_t tool_count, void *user)
{
	struct test_state *st = user;

	st->ready_calls++;
	st->ready_status = status;
	st->tool_count = tool_count;
}

static void
on_permission(const struct clm_permission_req *req, void *user)
{
	struct test_state *st = user;

	CHECK(clm_tool_permission_respond(st->agent, req,
	    CLM_PERM_ALLOW_ONCE) == 0, "allow mcp tool");
}

static void
on_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	struct test_state *st = user;

	(void)name;
	(void)content;
	st->tool_results++;
	st->tool_outcome = outcome;
}

static const struct clm_callbacks callbacks = {
	.on_permission = on_permission,
	.on_tool_result = on_tool_result,
};

static struct clm_agent *
make_agent(struct test_state *st)
{
	struct clm_cfg cfg = {
		.api_key = "",
		.base_url = "http://unused/v1/chat/completions",
		.model = "test-model",
	};
	struct clm_agent *agent = NULL;

	CHECK(clm_agent_new(&cfg, &host, &callbacks, st, &agent) == 0,
	    "create agent");
	st->agent = agent;
	return agent;
}

static struct clm_mcp_client *
connect_client(struct test_state *st, uv_loop_t *loop)
{
	struct clm_mcp_server_cfg cfg = {
		.name = "test",
		.transport = CLM_MCP_HTTP,
		.url = "http://unused/mcp",
	};
	struct clm_mcp_client *client = NULL;

	CHECK(clm_mcp_connect(st->agent, loop, &cfg, on_ready, st,
	    &client) == 0, "connect mcp client");
	CHECK(client != NULL, "mcp client returned");
	return client;
}

static void
test_free_pending_initialize(void)
{
	uv_loop_t loop;
	struct test_state st = {0};
	struct clm_mcp_client *client;

	mock_reset();
	CHECK(uv_loop_init(&loop) == 0, "initialize loop");
	st.agent = make_agent(&st);
	client = connect_client(&st, &loop);
	CHECK(mock_request_count == 1, "initialize request pending");

	clm_mcp_client_free(client);
	CHECK(mock_cancel_count == 1, "initialize request cancelled");
	clm_agent_free(st.agent);
	mock_error(mock_requests[0]);
	CHECK(st.ready_calls == 0, "initialize callback suppressed");
	CHECK(uv_loop_close(&loop) == 0, "close loop");
}

static void
test_free_pending_tools_list(void)
{
	uv_loop_t loop;
	struct test_state st = {0};
	struct clm_mcp_client *client;

	mock_reset();
	CHECK(uv_loop_init(&loop) == 0, "initialize loop");
	st.agent = make_agent(&st);
	client = connect_client(&st, &loop);
	mock_success(mock_requests[0],
	    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
	CHECK(mock_request_count == 2, "tools list request pending");

	clm_mcp_client_free(client);
	CHECK(mock_cancel_count == 1, "tools list request cancelled");
	clm_agent_free(st.agent);
	mock_error(mock_requests[1]);
	CHECK(st.ready_calls == 0, "tools list callback suppressed");
	CHECK(uv_loop_close(&loop) == 0, "close loop");
}

static cJSON *
tool_calls(void)
{
	return cJSON_Parse(
	    "[{\"id\":\"call-1\",\"type\":\"function\",\"function\":"
	    "{\"name\":\"mcp__test__ping\",\"arguments\":\"{}\"}},"
	    "{\"id\":\"call-2\",\"type\":\"function\",\"function\":"
	    "{\"name\":\"mcp__test__ping\",\"arguments\":\"{}\"}}]");
}

static void
test_free_pending_tool_call(void)
{
	uv_loop_t loop;
	struct test_state st = {0};
	struct clm_mcp_client *client;
	cJSON *calls;

	mock_reset();
	CHECK(uv_loop_init(&loop) == 0, "initialize loop");
	st.agent = make_agent(&st);
	client = connect_client(&st, &loop);
	mock_success(mock_requests[0],
	    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
	mock_success(mock_requests[1],
	    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
	    "{\"name\":\"ping\",\"description\":\"ping\",\"inputSchema\":"
	    "{\"type\":\"object\"}}]}}");
	CHECK(st.ready_calls == 1 && st.ready_status == 0 && st.tool_count == 1,
	    "mcp tool registered");

	calls = tool_calls();
	CHECK(calls != NULL, "parse tool call");
	CHECK(clm_tools_dispatch(st.agent, calls) == 0, "dispatch mcp tool");
	cJSON_Delete(calls);
	CHECK(mock_request_count == 4, "tool call requests pending");

	clm_mcp_client_free(client);
	CHECK(mock_cancel_count == 2, "tool call requests cancelled");
	CHECK(st.tool_results == 2 && st.tool_outcome == CLM_TOOL_FAILED,
	    "tool calls failed during free");
	clm_agent_free(st.agent);
	mock_error(mock_requests[2]);
	mock_error(mock_requests[3]);
	CHECK(st.tool_results == 2, "late tool callbacks suppressed");
	CHECK(uv_loop_close(&loop) == 0, "close loop");
}

int
main(void)
{
	test_free_pending_initialize();
	test_free_pending_tools_list();
	test_free_pending_tool_call();

	if (failures != 0) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
