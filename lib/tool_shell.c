// SPDX-License-Identifier: ISC
/*
 * shell_exec builtin — run a shell command via uv_spawn ($SHELL -c <command>).
 *
 * This is part of the desktop uv/curl layer (libclmuv), NOT the portable core:
 * a subprocess needs libuv, so keeping it here is what lets libclm itself stay
 * free of libuv. It talks to the core through the public tool API only
 * (clm_tool_* accessors), so it links against libclm without reaching into any
 * core internals. Register it with clm_tools_register_shell() after
 * clm_tools_register_builtins().
 */
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/host_uv.h"
#include "clm/cleanup.h"
#include "banned.h"

#define CLM_SHELL_DEFAULT_TIMEOUT_MS 30000u

/* Local copy of the core's arg_string helper (kept private so libclmuv depends
 * only on libclm's public API). */
static char *
sh_arg_string(struct json_object *args, const char *key)
{
	struct json_object *v = NULL;
	if (!json_object_object_get_ex(args, key, &v))
		return NULL;
	if (json_object_get_type(v) != json_type_string)
		return NULL;
	return strdup(json_object_get_string(v));
}

/* Shell exec via uv_spawn ($SHELL -c <command>). */
struct shell_state {
	struct clm_tool_invocation *inv;
	uv_process_t proc;
	uv_pipe_t in;         /* present only when stdin is supplied */
	uv_pipe_t out;
	uv_pipe_t err;
	uv_write_t wreq;
	char *in_buf;         /* stdin blob, kept alive across the write */
	bool has_stdin;
	char *buf;
	size_t len;
	size_t bufcap;
	int handles;          /* uv handles still open (proc + pipes) */
	int64_t exit_status;
	int term_signal;
	char *spawn_err;
};

static void
shell_append(struct shell_state *s, const char *data, size_t n)
{
	size_t cap = clm_tool_invocation_output_cap(s->inv);
	size_t room, take;

	if (s->len >= cap)
		return; /* full; drain and discard the rest */
	room = cap - s->len;
	take = n < room ? n : room;

	if (s->len + take + 1 > s->bufcap) {
		size_t nc = s->bufcap ? s->bufcap * 2 : 4096;
		char *p;
		while (nc < s->len + take + 1)
			nc *= 2;
		if (nc > cap + 1)
			nc = cap + 1;
		p = realloc(s->buf, nc);
		if (p == NULL)
			return;
		s->buf = p;
		s->bufcap = nc;
	}
	memcpy(s->buf + s->len, data, take);
	s->len += take;
	s->buf[s->len] = '\0';
}

static void
shell_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static void
shell_finish(struct shell_state *s)
{
	struct clm_tool_invocation *inv = s->inv;

	if (s->spawn_err != NULL) {
		clm_tool_fail(inv, s->spawn_err);
	} else if (s->exit_status != 0 || s->term_signal != 0) {
		size_t mlen = s->len + 80;
		autofree char *msg = malloc(mlen);
		if (msg != NULL) {
			(void)snprintf(msg, mlen, "%s%s(exit status %lld%s)",
			    s->len ? s->buf : "", s->len ? "\n" : "",
			    (long long)s->exit_status,
			    s->term_signal ? ", killed by signal" : "");
			clm_tool_fail(inv, msg);
		} else {
			clm_tool_fail(inv, "command failed");
		}
	} else {
		clm_tool_complete(inv, s->len ? s->buf : "(command produced no output)");
	}

	free(s->spawn_err);
	free(s->in_buf);
	free(s->buf);
	free(s);
}

static void
shell_on_close(uv_handle_t *handle)
{
	struct shell_state *s = handle->data;
	if (--s->handles == 0)
		shell_finish(s);
}

static void
shell_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct shell_state *s = stream->data;
	if (nread > 0)
		shell_append(s, buf->base, (size_t)nread);
	else if (nread < 0)
		uv_close((uv_handle_t *)stream, shell_on_close);
	free(buf->base);
}

static void
shell_on_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct shell_state *s = proc->data;
	s->exit_status = exit_status;
	s->term_signal = term_signal;
	uv_close((uv_handle_t *)proc, shell_on_close);
}

static void
shell_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct shell_state *s = user;
	(void)inv;
	uv_process_kill(&s->proc, SIGTERM);
}

/* stdin blob written: close the pipe so the child sees EOF. */
static void
shell_on_stdin_written(uv_write_t *req, int status)
{
	struct shell_state *s = req->data;
	(void)status;
	uv_close((uv_handle_t *)&s->in, shell_on_close);
}

static void
tool_shell_exec(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup struct json_object *args =
	    json_tokener_parse(clm_tool_invocation_args(inv));
	autofree char *command = NULL;
	autoclose int devnull = -1;
	struct shell_state *s;
	uv_loop_t *loop = clm_tool_invocation_loop(inv);
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	const char *shell;
	char *argv[4];
	int r;

	(void)user;
	if (args == NULL || json_object_get_type(args) != json_type_object) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = sh_arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(inv, "missing required string argument 'command'");
		return;
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	s->inv = inv;
	s->proc.data = s;
	uv_pipe_init(loop, &s->out, 0);
	s->out.data = s;
	uv_pipe_init(loop, &s->err, 0);
	s->err.data = s;

	/* Optional stdin blob: feed it through a pipe, else use /dev/null. */
	s->in_buf = sh_arg_string(args, "stdin");
	s->has_stdin = (s->in_buf != NULL);
	if (s->has_stdin) {
		uv_pipe_init(loop, &s->in, 0);
		s->in.data = s;
		stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
		stdio[0].data.stream = (uv_stream_t *)&s->in;
	} else {
		devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (devnull >= 0) {
			stdio[0].flags = UV_INHERIT_FD;
			stdio[0].data.fd = devnull;
		} else {
			stdio[0].flags = UV_IGNORE;
		}
	}

	shell = getenv("SHELL");
	if (shell == NULL || shell[0] == '\0')
		shell = "/bin/sh";
	argv[0] = (char *)shell;
	argv[1] = "-c";
	argv[2] = command;
	argv[3] = NULL;

	memset(&opt, 0, sizeof(opt));
	opt.file = shell;
	opt.args = argv;
	opt.exit_cb = shell_on_exit;
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&s->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&s->err;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	s->handles = s->has_stdin ? 4 : 3;
	r = uv_spawn(loop, &s->proc, &opt);
	if (r < 0) {
		s->spawn_err = strdup(uv_strerror(r));
		uv_close((uv_handle_t *)&s->proc, shell_on_close);
		uv_close((uv_handle_t *)&s->out, shell_on_close);
		uv_close((uv_handle_t *)&s->err, shell_on_close);
		if (s->has_stdin)
			uv_close((uv_handle_t *)&s->in, shell_on_close);
		return;
	}

	clm_tool_invocation_set_cancel(inv, shell_cancel, s);
	uv_read_start((uv_stream_t *)&s->out, shell_alloc, shell_read);
	uv_read_start((uv_stream_t *)&s->err, shell_alloc, shell_read);

	if (s->has_stdin) {
		uv_buf_t b = uv_buf_init(s->in_buf, (unsigned)strlen(s->in_buf));
		s->wreq.data = s;
		if (uv_write(&s->wreq, (uv_stream_t *)&s->in, &b, 1,
		    shell_on_stdin_written) < 0)
			uv_close((uv_handle_t *)&s->in, shell_on_close);
	}
}

int
clm_tools_register_shell(struct clm_agent *agent)
{
	const struct clm_tool_def shell_def = {
		.name = "shell_exec",
		.description = "execute a shell command and return its output",
		.params_schema =
		    "{\"type\":\"object\","
		    "\"properties\":{"
		    "\"command\":{\"type\":\"string\","
		    "\"description\":\"the shell command to execute\"},"
		    "\"stdin\":{\"type\":\"string\","
		    "\"description\":\"optional: data to write to the command's standard input\"}},"
		    "\"required\":[\"command\"]}",
		.invoke = tool_shell_exec,
		.timeout_ms = CLM_SHELL_DEFAULT_TIMEOUT_MS,
		.flags = CLM_TOOL_TIMEOUT_OVERRIDABLE | CLM_TOOL_OUTPUT_CAP_OVERRIDABLE,
	};
	return clm_tool_add(agent, &shell_def);
}
