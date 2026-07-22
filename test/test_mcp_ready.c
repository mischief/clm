// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "clm/clm.h"
#include "clm/host.h"
#include "clm/mcp.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                    \
		if (!(cond)) {                                                  \
			fprintf(stderr, "fail: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                          \
			failures++;                                            \
		}                                                               \
	} while (0)

struct test_state {
	struct clm_mcp_client *client;
	uv_timer_t timeout;
	int statuses[3];
	size_t status_count;
	int ready_count;
	int destroy_count;
	bool timed_out;
};

struct ready_ctx {
	struct test_state *state;
};

static int
test_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	(void)ctx;
	(void)req;
	(void)success;
	(void)error;
	(void)data;
	(void)user;
	(void)out;
	return -ENOSYS;
}

static void
ready_ctx_free(void *user)
{
	struct ready_ctx *ctx = user;

	ctx->state->destroy_count++;
	free(ctx);
}

static void
stop_client(struct test_state *state)
{
	struct clm_mcp_client *client = state->client;

	state->client = NULL;
	if (!uv_is_closing((uv_handle_t *)&state->timeout)) {
		uv_timer_stop(&state->timeout);
		uv_close((uv_handle_t *)&state->timeout, NULL);
	}
	clm_mcp_client_free(client);
}

static void
on_ready(int status, size_t tool_count, void *user)
{
	struct ready_ctx *ctx = user;
	struct test_state *state = ctx->state;

	(void)tool_count;
	if (state->status_count < sizeof(state->statuses) / sizeof(state->statuses[0]))
		state->statuses[state->status_count] = status;
	state->status_count++;
	if (status == 0 && ++state->ready_count == 2)
		stop_client(state);
}

static void
on_timeout(uv_timer_t *timer)
{
	struct test_state *state = timer->data;

	state->timed_out = true;
	stop_client(state);
}

static int
server_main(void)
{
	char line[4096];

	if (fgets(line, sizeof(line), stdin) == NULL)
		return 1;
	if (fputs("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n", stdout) < 0 ||
	    fflush(stdout) != 0)
		return 1;
	if (fgets(line, sizeof(line), stdin) == NULL)
		return 1;
	if (fputs("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[]}}\n",
	    stdout) < 0 || fflush(stdout) != 0)
		return 1;
	return 0;
}

static void
test_restart_callback_lifetime(const char *self_path)
{
	struct clm_host host = { .http_post = test_http_post };
	struct clm_cfg cfg = {
		.api_key = "test",
		.base_url = "http://127.0.0.1:1/v1/chat/completions",
		.model = "test",
	};
	struct clm_mcp_server_cfg server_cfg = {
		.name = "restart-test",
		.transport = CLM_MCP_STDIO,
	};
	struct test_state state = {0};
	struct ready_ctx *ctx;
	struct clm_agent *agent = NULL;
	uv_loop_t loop;
	char *server_argv[] = { (char *)self_path, "--server", NULL };
	int r;

	server_cfg.argv = server_argv;
	CHECK(uv_loop_init(&loop) == 0, "loop initialization");
	CHECK(clm_agent_new(&cfg, &host, NULL, NULL, &agent) == 0,
	    "agent creation");
	if (agent == NULL) {
		uv_loop_close(&loop);
		return;
	}
	CHECK(uv_timer_init(&loop, &state.timeout) == 0, "timeout initialization");
	state.timeout.data = &state;

	ctx = calloc(1, sizeof(*ctx));
	CHECK(ctx != NULL, "callback context allocation");
	if (ctx == NULL) {
		uv_close((uv_handle_t *)&state.timeout, NULL);
		uv_run(&loop, UV_RUN_DEFAULT);
		clm_agent_free(agent);
		uv_loop_close(&loop);
		return;
	}
	ctx->state = &state;
	r = clm_mcp_connect(agent, &loop, &server_cfg, on_ready, ctx,
	    ready_ctx_free, &state.client);
	CHECK(r == 0, "mcp connection start");
	if (r != 0) {
		free(ctx);
		uv_close((uv_handle_t *)&state.timeout, NULL);
	} else {
		uv_timer_start(&state.timeout, on_timeout, 5000, 0);
	}

	uv_run(&loop, UV_RUN_DEFAULT);
	if (state.client != NULL)
		stop_client(&state);
	uv_run(&loop, UV_RUN_DEFAULT);

	CHECK(!state.timed_out, "stdio server restarted before timeout");
	CHECK(state.status_count == 3, "three status callbacks received");
	if (state.status_count >= 3) {
		CHECK(state.statuses[0] == 0, "initial ready status");
		CHECK(state.statuses[1] < 0, "disconnect status");
		CHECK(state.statuses[2] == 0, "restart ready status");
	}
	CHECK(state.destroy_count == 1, "callback context destroyed once");

	clm_agent_free(agent);
	CHECK(uv_loop_close(&loop) == 0, "loop close");
}

int
main(int argc, char **argv)
{
	char *self_path;

	if (argc == 2 && strcmp(argv[1], "--server") == 0)
		return server_main();

	self_path = realpath(argv[0], NULL);
	CHECK(self_path != NULL, "resolve test executable path");
	if (self_path != NULL) {
		test_restart_callback_lifetime(self_path);
		free(self_path);
	}
	return failures == 0 ? 0 : 1;
}
