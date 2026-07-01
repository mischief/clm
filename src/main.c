// SPDX-License-Identifier: ISC
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "clm/clm.h"
#include "frontend.h"

#ifdef CLM_LUA
#include "clm/lua_plugin.h"
#endif

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "usage: %s [-o|--oneshot PROMPT] [-H|--headless] [-u|--url BASE] "
	    "[-m|--model NAME] [-S|--no-stream] [-h|--help]\n",
	    prog);
	fprintf(stderr,
	    "  -o, --oneshot PROMPT  run one prompt headless and exit\n"
	    "  -H, --headless        force the plain stdio REPL\n"
	    "  -u, --url BASE        base API endpoint "
	    "(default http://127.0.0.1:8081);\n"
	    "                        \"/v1/chat/completions\" is appended\n"
	    "  -m, --model NAME      model name to request\n"
	    "  -S, --no-stream       disable streamed (SSE) responses\n"
	    "  -h, --help            show this help\n"
	    "\n"
	    "  With no options it runs the interactive ncurses UI on a "
	    "terminal.\n");
}

struct cli_state {
	uv_loop_t *loop;
	struct clm_agent *agent;
#ifdef CLM_LUA
	struct clm_lua_env *lua_env;
#endif
	uv_pipe_t stdin_pipe;
	char prompt_line[1024];
	size_t prompt_len;
	int oneshot;
	int turn_done;
	int turn_status;
};

static void
cb_assistant_text(const char *text, void *user)
{
	struct cli_state *state = (struct cli_state *)user;
	if (state->oneshot)
		printf("%s", text);
	else
		printf("assistant> %s", text);
	fflush(stdout);
}

static void
cb_reasoning(const char *text, void *user)
{
	(void)user;
	printf("\033[2m%s\033[0m", text); /* dim "thinking" channel */
	fflush(stdout);
}

static void
cb_finish_reason(enum clm_finish_reason reason, void *user)
{
	(void)user;
	if (reason == CLM_FINISH_LENGTH)
		printf("\n\033[33m[truncated: hit token limit]\033[0m\n");
	else if (reason == CLM_FINISH_CONTENT_FILTER)
		printf("\n\033[31m[stopped: content filter]\033[0m\n");
	fflush(stdout);
}

static void
cb_usage(const struct clm_usage *usage, void *user)
{
	(void)user;
	printf("\033[2m[%d+%d tok", usage->prompt_tokens, usage->completion_tokens);
	if (usage->tokens_per_sec > 0)
		printf(", %.1f tok/s", usage->tokens_per_sec);
	printf("]\033[0m\n");
	fflush(stdout);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	(void)user;
	printf("[tool: %s %s]\n", name, args ? args : "");
	fflush(stdout);
}

static void
cb_tool_result(const char *name, const char *content, enum clm_tool_outcome outcome, void *user)
{
	(void)user;
	switch (outcome) {
	case CLM_TOOL_OK:
		printf("[result: %s]\n%s\n", name, content ? content : "");
		break;
	case CLM_TOOL_FAILED:
		printf("\033[31m[x %s]\033[0m %s\n", name, content ? content : "");
		break;
	case CLM_TOOL_TIMEDOUT:
		printf("\033[33m[timeout %s]\033[0m %s\n", name, content ? content : "");
		break;
	}
	fflush(stdout);
}

static void
cb_tool_batch(size_t completed, size_t total, void *user)
{
	(void)user;
	printf("[tools %zu/%zu]\n", completed, total);
	fflush(stdout);
}

static void
cb_state(enum clm_agent_state state, void *user)
{
}

static void
cb_turn_done(int status, void *user)
{
	struct cli_state *state = (struct cli_state *)user;
	if (status != 0) {
		fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
	}
	if (state->oneshot) {
		state->turn_done = 1;
		state->turn_status = status;
		return;
	}
	printf("\nuser> ");
	fflush(stdout);
}

static const struct clm_callbacks cli_callbacks = {
	.on_assistant_text = cb_assistant_text,
	.on_reasoning = cb_reasoning,
	.on_tool_begin = cb_tool_begin,
	.on_tool_result = cb_tool_result,
	.on_tool_batch = cb_tool_batch,
	.on_finish_reason = cb_finish_reason,
	.on_usage = cb_usage,
	.on_state = cb_state,
	.on_turn_done = cb_turn_done,
};

static void
on_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = malloc(suggested_size);
	if (buf->base == NULL) {
		buf->len = 0;
		return;
	}
	buf->len = suggested_size;
}

static void
on_stdin_close(uv_handle_t *handle)
{
	struct cli_state *state = (struct cli_state *)handle->data;
	uv_stop(state->loop);
}

static void
on_stdin_read(uv_stream_t *stream, ssize_t n_read, const uv_buf_t *buf)
{
	struct cli_state *state = (struct cli_state *)stream->data;

	if (n_read < 0) {
		if (n_read == UV_ENOBUFS)
			return;
		if (n_read == UV_EOF) {
			free(buf->base);
			printf("\n");
			fflush(stdout);
			uv_close((uv_handle_t *)stream, on_stdin_close);
			return;
		}
		fprintf(stderr, "read error: %s\n", uv_err_name(n_read));
		uv_close((uv_handle_t *)stream, on_stdin_close);
		return;
	}

	if (n_read == 0) {
		free(buf->base);
		return;
	}

	for (ssize_t i = 0; i < n_read; i++) {
		if (state->prompt_len < sizeof(state->prompt_line) - 1) {
			state->prompt_line[state->prompt_len++] = buf->base[i];
		}
		if (buf->base[i] == '\n' || buf->base[i] == '\r') {
			if (state->prompt_len > 0 && state->prompt_line[state->prompt_len - 1] == '\r')
				state->prompt_len--;
			if (state->prompt_len > 0 && state->prompt_line[state->prompt_len - 1] == '\n')
				state->prompt_len--;
			state->prompt_line[state->prompt_len] = '\0';

			if (state->prompt_len > 0) {
				if (strcmp(state->prompt_line, "quit") == 0 || strcmp(state->prompt_line, "exit") == 0) {
					free(buf->base);
					uv_close((uv_handle_t *)stream, on_stdin_close);
					return;
				}

				int r = clm_agent_submit(state->agent, state->prompt_line);
				if (r < 0) {
					fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
				}
			}

			state->prompt_len = 0;
			break;
		}
	}

	free(buf->base);
}

int
main(int argc, char *argv[])
{
	const char *api_base = "http://127.0.0.1:8081";
	const char *model = "local-model";
	char *oneshot = NULL;
	int stream = 1;
	int headless = 0;
	struct clm_cfg cfg = {0};
	struct cli_state *state;
	uv_loop_t *loop;
	char endpoint[256];
	size_t baselen;
	int opt, r;

	const struct option opts[] = {
		{"oneshot", required_argument, NULL, 'o'},
		{"url", required_argument, NULL, 'u'},
		{"model", required_argument, NULL, 'm'},
		{"headless", no_argument, NULL, 'H'},
		{"no-stream", no_argument, NULL, 'S'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "o:u:m:HSh", opts, NULL)) != -1) {
		switch (opt) {
		case 'o': oneshot = optarg; break;
		case 'u': api_base = optarg; break;
		case 'm': model = optarg; break;
		case 'H': headless = 1; break;
		case 'S': stream = 0; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}

	/* -u takes the base endpoint; build the chat-completions URL from it,
	 * tolerating a trailing slash on the base. */
	baselen = strlen(api_base);
	while (baselen > 0 && api_base[baselen - 1] == '/')
		baselen--;
	snprintf(endpoint, sizeof(endpoint), "%.*s/v1/chat/completions",
	    (int)baselen, api_base);

	cfg.api_key = "sk-no-key-required";
	cfg.base_url = endpoint;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = model;
	cfg.max_iterations = 25;
	cfg.stream = stream;

	/*
	 * Default to the ncurses UI when we're interactive: no oneshot, not
	 * forced headless, and stdin+stdout are both a terminal. Otherwise fall
	 * through to the plain stdio path (works for pipes and --oneshot).
	 */
	if (oneshot == NULL && !headless && isatty(STDIN_FILENO) &&
	    isatty(STDOUT_FILENO))
		return tui_run(&cfg);

	/* Heap-allocated so the 1 KB input buffer stays off main's stack frame. */
	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}

	loop = uv_default_loop();
	state->loop = loop;

	state->oneshot = (oneshot != NULL);

	r = clm_agent_new(&cfg, loop, &cli_callbacks, state, &state->agent);
	if (r < 0) {
		fprintf(stderr, "error: failed to create agent (%d)\n", r);
		free(state);
		return 1;
	}

#ifdef CLM_LUA
	if (clm_lua_env_new(state->agent, &state->lua_env) == 0)
		clm_lua_load_plugins(state->lua_env, "plugins");
#endif

	if (oneshot != NULL) {
		r = clm_agent_submit(state->agent, oneshot);
		if (r < 0) {
			fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
#ifdef CLM_LUA
			clm_lua_env_free(state->lua_env);
#endif
			clm_agent_free(state->agent);
			free(state);
			return 1;
		}

		while (!state->turn_done)
			uv_run(loop, UV_RUN_ONCE);

		printf("\n");
#ifdef CLM_LUA
		clm_lua_env_free(state->lua_env);
#endif
		clm_agent_free(state->agent);
		r = state->turn_status;
		free(state);
		return r == 0 ? 0 : 1;
	}

	printf("clm agent. api: %s\n", endpoint);
	printf("type 'quit' or 'exit' to stop.\n\n");
	printf("user> ");
	fflush(stdout);

	uv_pipe_init(loop, &state->stdin_pipe, 0);
	uv_pipe_open(&state->stdin_pipe, fileno(stdin));
	state->stdin_pipe.data = (uv_handle_t *)state;
	uv_read_start((uv_stream_t *)&state->stdin_pipe, on_alloc_buffer, on_stdin_read);

	uv_run(loop, UV_RUN_DEFAULT);

#ifdef CLM_LUA
	clm_lua_env_free(state->lua_env);
#endif
	clm_agent_free(state->agent);
	free(state);

	return 0;
}
