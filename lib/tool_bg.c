// SPDX-License-Identifier: ISC
/*
 * bg_exec builtin — start a shell command via uv_spawn ($SHELL -c <command>)
 * and return to the model immediately, instead of blocking the turn until
 * the command exits (that's what shell_exec is for). The eventual result is
 * delivered later via clm_agent_notify(), as a fresh turn rather than as
 * this tool call's own result -- by the time the command exits, the
 * invocation that started it is long since complete, so there is no pending
 * tool_call_id left to attach a result to (every chat-completions-style API
 * requires each tool_call in an assistant message to be answered before the
 * next request, so a result literally cannot arrive late on the same
 * tool_call_id; it has to become a new turn instead).
 *
 * Like tool_shell.c, this lives in the desktop uv/curl layer (libclmuv), not
 * the portable core: a subprocess needs libuv. Register it with
 * clm_tools_register_bg() alongside clm_tools_register_shell().
 *
 * Known limitation: a job still running when the agent is torn down is not
 * killed or waited on here (no teardown hook wired up yet) -- the child
 * becomes an orphan of the process, same as it would with a bare fork/exec
 * left to run past its parent's exit. Fine for a REPL session ending
 * normally; worth revisiting if this is ever used somewhere longer-lived.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/host_uv.h"
#include "clm/cleanup.h"
#include "banned.h"

/* Output captured per job before it's folded into the eventual notify
 * message; independent of shell_exec's per-call output_cap since there is no
 * invocation left by the time this matters to clamp against. */
#define CLM_BG_OUTPUT_CAP (16 * 1024)

/* Local copy of the core's arg_string helper (kept private so libclmuv
 * depends only on libclm's public API; tool_shell.c has its own copy for the
 * same reason). */
static char *
bg_arg_string(cJSON *args, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
	if (v == NULL || !cJSON_IsString(v))
		return NULL;
	return strdup(cJSON_GetStringValue(v));
}

struct clm_bg_job {
	uint64_t id;
	struct clm_agent *agent; /* where the eventual result is delivered */
	char *label;             /* the "label" arg, or the command if none given */

	uv_process_t proc;
	uv_pipe_t out, err;
	char *buf;
	size_t len, bufcap;

	int handles; /* proc + out + err; job frees itself at 0 */
	int64_t exit_status;
	int term_signal;

	/* False if uv_spawn itself failed: the invocation already got a
	 * synchronous clm_tool_fail() for that, so bg_finish must not also
	 * send a notify -- that would report the same failure twice. */
	bool started;

	TAILQ_ENTRY(clm_bg_job) entries;
};

TAILQ_HEAD(clm_bg_job_list, clm_bg_job);
static struct clm_bg_job_list bg_jobs = TAILQ_HEAD_INITIALIZER(bg_jobs);
static uint64_t bg_next_id = 1;

/* Same growth strategy as tool_shell.c's shell_append, capped at
 * CLM_BG_OUTPUT_CAP instead of a per-invocation output_cap. */
static void
bg_append(struct clm_bg_job *j, const char *data, size_t n)
{
	size_t room, take;

	if (j->len >= CLM_BG_OUTPUT_CAP)
		return; /* full; drain and discard the rest */
	room = CLM_BG_OUTPUT_CAP - j->len;
	take = n < room ? n : room;

	if (j->len + take + 1 > j->bufcap) {
		size_t nc = j->bufcap ? j->bufcap * 2 : 4096;
		char *p;
		while (nc < j->len + take + 1)
			nc *= 2;
		if (nc > CLM_BG_OUTPUT_CAP + 1)
			nc = CLM_BG_OUTPUT_CAP + 1;
		p = realloc(j->buf, nc);
		if (p == NULL)
			return;
		j->buf = p;
		j->bufcap = nc;
	}
	memcpy(j->buf + j->len, data, take);
	j->len += take;
	j->buf[j->len] = '\0';
}

static void
bg_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static void
bg_finish(struct clm_bg_job *j)
{
	/* uv_spawn itself failed: clm_tool_fail() already reported this
	 * synchronously to the invocation, before the job was even started.
	 * Nothing more to deliver -- just release the uv handles' bookkeeping. */
	if (j->started) {
		autofree char *msg = NULL;

		(void)asprintf(&msg,
		    "[background job %llu (\"%s\") finished, exit status %lld%s]\n%s",
		    (unsigned long long)j->id, j->label, (long long)j->exit_status,
		    j->term_signal ? ", killed by signal" : "",
		    j->len ? j->buf : "(no output)");
		/* On OOM building msg: drop the notification silently. The
		 * process itself already ran to completion -- losing the
		 * notification loses visibility, not correctness, and there is
		 * no pending tool_call_id left to report a failure through
		 * (see the file comment). */
		if (msg != NULL)
			(void)clm_agent_notify(j->agent, msg);
	}

	TAILQ_REMOVE(&bg_jobs, j, entries);
	free(j->buf);
	free(j->label);
	free(j);
}

static void
bg_on_close(uv_handle_t *handle)
{
	struct clm_bg_job *j = handle->data;
	if (--j->handles == 0)
		bg_finish(j);
}

static void
bg_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct clm_bg_job *j = stream->data;
	if (nread > 0)
		bg_append(j, buf->base, (size_t)nread);
	else if (nread < 0)
		uv_close((uv_handle_t *)stream, bg_on_close);
	free(buf->base);
}

static void
bg_on_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct clm_bg_job *j = proc->data;
	j->exit_status = exit_status;
	j->term_signal = term_signal;
	uv_close((uv_handle_t *)proc, bg_on_close);
}

static void
tool_bg_exec(struct clm_tool_invocation *inv, void *user)
{
	struct clm_agent *agent = user;
	json_cleanup cJSON *args =
	    cJSON_Parse(clm_tool_invocation_args(inv));
	autofree char *command = NULL;
	autofree char *label = NULL;
	autofree char *started_msg = NULL;
	struct clm_bg_job *j;
	uv_loop_t *loop = clm_tool_invocation_loop(inv);
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	const char *shell;
	char *argv[4];
	int r;

	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = bg_arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(inv, "missing required string argument 'command'");
		return;
	}
	label = bg_arg_string(args, "label");

	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	j->agent = agent;
	j->id = bg_next_id++;
	j->label = strdup(label != NULL ? label : command);
	if (j->label == NULL) {
		free(j);
		clm_tool_fail(inv, "out of memory");
		return;
	}

	j->proc.data = j;
	uv_pipe_init(loop, &j->out, 0);
	j->out.data = j;
	uv_pipe_init(loop, &j->err, 0);
	j->err.data = j;

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
	opt.exit_cb = bg_on_exit;
	stdio[0].flags = UV_IGNORE; /* no stdin for a detached background job */
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&j->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&j->err;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	/* Insert before spawning either way: bg_on_close/bg_finish run off the
	 * uv_close callbacks below regardless of whether uv_spawn succeeds, and
	 * bg_finish always does a TAILQ_REMOVE. j->started (still false here)
	 * is what tells bg_finish whether that failure was already reported
	 * synchronously via clm_tool_fail below (no notify) or needs its usual
	 * exit-status notify. */
	TAILQ_INSERT_TAIL(&bg_jobs, j, entries);

	j->handles = 3;
	r = uv_spawn(loop, &j->proc, &opt);
	if (r < 0) {
		uv_close((uv_handle_t *)&j->proc, bg_on_close);
		uv_close((uv_handle_t *)&j->out, bg_on_close);
		uv_close((uv_handle_t *)&j->err, bg_on_close);
		clm_tool_fail(inv, uv_strerror(r));
		return;
	}
	j->started = true;

	uv_read_start((uv_stream_t *)&j->out, bg_alloc, bg_read);
	uv_read_start((uv_stream_t *)&j->err, bg_alloc, bg_read);

	(void)asprintf(&started_msg,
	    "started background job %llu: %s (result will arrive later as a "
	    "new message, not as this call's result)",
	    (unsigned long long)j->id, j->label);
	clm_tool_complete(inv, started_msg != NULL ? started_msg
	                                            : "started background job");
}

int
clm_tools_register_bg(struct clm_agent *agent)
{
	const struct clm_tool_def bg_def = {
		.name = "bg_exec",
		.description =
		    "start a shell command running in the background and return "
		    "immediately with a job id, instead of waiting for it to finish "
		    "(use shell_exec for that). The command's real output arrives "
		    "later as a separate message tagged with the same job id, not "
		    "as this call's result -- do not wait for it, continue with "
		    "other work.",
		.params_schema =
		    "{\"type\":\"object\","
		    "\"properties\":{"
		    "\"command\":{\"type\":\"string\","
		    "\"description\":\"the shell command to run in the background\"},"
		    "\"label\":{\"type\":\"string\","
		    "\"description\":\"optional short label to identify this job in "
		    "its later result message; defaults to the command itself\"}},"
		    "\"required\":[\"command\"]}",
		.invoke = tool_bg_exec,
		.user = agent,
	};
	return clm_tool_add(agent, &bg_def);
}
