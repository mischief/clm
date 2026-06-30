// SPDX-License-Identifier: ISC
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "clm/clm.h"

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--oneshot PROMPT] [--url URL] [--model NAME]\n", prog);
}

struct cli_state {
	uv_loop_t *loop;
	struct clm_agent *agent;
	uv_pipe_t stdin_pipe;
	char prompt_line[1024];
	size_t prompt_len;
};

static void
cb_assistant_text(const char *text, void *user)
{
}

static void
cb_reasoning(const char *text, void *user)
{
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
}

static void
cb_tool_result(const char *name, const char *content, void *user)
{
}

static void
cb_state(enum clm_agent_state state, void *user)
{
}

static void
cb_turn_done(int status, void *user)
{
}

static const struct clm_callbacks cli_callbacks = {
	.on_assistant_text = cb_assistant_text,
	.on_reasoning = cb_reasoning,
	.on_tool_begin = cb_tool_begin,
	.on_tool_result = cb_tool_result,
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
on_stdin_read(uv_stream_t *stream, ssize_t n_read, const uv_buf_t *buf)
{
	struct cli_state *state = (struct cli_state *)stream->data;

	if (n_read < 0) {
		if (n_read == UV_ENOBUFS)
			return;
		if (n_read != UV_EOF)
			fprintf(stderr, "read error: %s\n", uv_err_name(n_read));
		uv_close((uv_handle_t *)stream, NULL);
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
					uv_close((uv_handle_t *)stream, NULL);
					return;
				}

				char *result = NULL;
				int r = clm_agent_run(state->agent, state->prompt_line, &result);
				if (r < 0) {
					fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
				} else {
					printf("assistant> %s\n\n", result);
					free(result);
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
	const char *api_url = "http://127.0.0.1:8081/v1/chat/completions";
	const char *model = "local-model";
	char *oneshot = NULL;
	struct clm_cfg cfg = {0};
	struct cli_state state = {0};
	uv_loop_t *loop;
	int opt, r;

	const struct option opts[] = {
		{"oneshot", required_argument, NULL, 'o'},
		{"url", required_argument, NULL, 'u'},
		{"model", required_argument, NULL, 'm'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "o:u:m:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'o': oneshot = optarg; break;
		case 'u': api_url = optarg; break;
		case 'm': model = optarg; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}

	cfg.api_key = "sk-no-key-required";
	cfg.base_url = api_url;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = model;
	cfg.max_iterations = 25;

	loop = uv_default_loop();
	state.loop = loop;

	r = clm_agent_new(&cfg, loop, &cli_callbacks, &state, &state.agent);
	if (r < 0) {
		fprintf(stderr, "error: failed to create agent (%d)\n", r);
		return 1;
	}

	if (oneshot != NULL) {
		char *result = NULL;
		r = clm_agent_run(state.agent, oneshot, &result);
		if (r < 0) {
			fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state.agent));
			return 1;
		}
		printf("%s\n", result);
		free(result);
		return 0;
	}

	printf("clm agent. api: %s\n", api_url);
	printf("type 'quit' or 'exit' to stop.\n\n");

	uv_pipe_init(loop, &state.stdin_pipe, 0);
	uv_pipe_open(&state.stdin_pipe, fileno(stdin));
	state.stdin_pipe.data = (uv_handle_t *)&state;
	uv_read_start((uv_stream_t *)&state.stdin_pipe, on_alloc_buffer, on_stdin_read);

	uv_run(loop, UV_RUN_DEFAULT);

	clm_agent_free(state.agent);

	return 0;
}
