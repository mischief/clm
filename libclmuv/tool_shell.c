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

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/host_uv.h"
#include "clm/cleanup.h"
#include "banned.h"

#define CLM_SHELL_DEFAULT_TIMEOUT_MS 30000u

/*
 * Grace period after shell_cancel()'s SIGTERM before we stop waiting for the
 * child's own pipes to close and force them closed ourselves. Covers the
 * case documented in shell_cancel(): a backgrounded grandchild ("cmd &")
 * outlives the killed $SHELL -c process and keeps holding the stdout/stderr
 * pipes open, so shell_read() never sees EOF and shell_finish() never runs --
 * the tool call (and the whole batch it belongs to) hangs forever. Forcing
 * the pipes closed here guarantees shell_finish() always runs within
 * timeout_ms + this grace period, regardless of what the child does.
 */
#define CLM_SHELL_KILL_GRACE_MS_DEFAULT 5000u

/*
 * Overridable via CLM_SHELL_KILL_GRACE_MS so tests exercising the escalation
 * path (a grandchild that ignores SIGTERM entirely) don't have to actually
 * wait out the production default. Read once and cached: the env var isn't
 * meant to change mid-run.
 */
static uint64_t
shell_kill_grace_ms(void)
{
	static uint64_t ms;
	static bool init;
	const char *e;

	if (init)
		return ms;
	init = true;
	ms = CLM_SHELL_KILL_GRACE_MS_DEFAULT;
	if ((e = getenv("CLM_SHELL_KILL_GRACE_MS")) != NULL) {
		char *end;
		unsigned long long v = strtoull(e, &end, 10);
		if (end != e && *end == '\0')
			ms = (uint64_t)v;
	}
	return ms;
}

/* Local copy of the core's arg_string helper (kept private so libclmuv depends
 * only on libclm's public API). */
static char *
sh_arg_string(cJSON *args, const char *key)
{
	cJSON *v = NULL;
	if (!(v = cJSON_GetObjectItemCaseSensitive(args, key)))
		return NULL;
	if (!cJSON_IsString(v))
		return NULL;
	return strdup(v->valuestring);
}

/* Shell exec via uv_spawn ($SHELL -c <command>). */
struct shell_state {
	struct clm_tool_invocation *inv;
	uv_process_t proc;
	uv_pipe_t in; /* present only when stdin is supplied */
	uv_pipe_t out;
	uv_pipe_t err;
	uv_write_t wreq;
	char *in_buf; /* stdin blob, kept alive across the write */
	bool has_stdin;
	char *buf;
	size_t len;
	size_t bufcap;
	int handles; /* uv handles still open (proc + pipes [+ kill_timer]) */
	int64_t exit_status;
	int term_signal;
	char *spawn_err;

	/* Grace-period escalation after shell_cancel(): see
	 * CLM_SHELL_KILL_GRACE_MS and shell_kill_timer_cb(). Counted as one
	 * more entry in `handles` from the moment it's armed, so shell_finish
	 * cannot fire (and free this struct) while the timer is still live on
	 * the loop. */
	uv_timer_t kill_timer;
	bool kill_timer_armed;
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
		size_t mlen = s->len + 128;
		autofree char *msg = malloc(mlen);
		const char *body = s->len ? s->buf : "(no output)";
		/* Command output usually ends in its own newline; do not add
		 * a second one before the status line. */
		const char *sep =
		    (s->len && s->buf[s->len - 1] == '\n') ? "" : "\n";

		if (msg != NULL) {
			/* A signal-terminated process has no real exit status:
			 * uv_process_t reports 0 there, so print the signal
			 * instead of a meaningless "exit status 0". */
			if (s->term_signal != 0) {
				const char *signame = strsignal(s->term_signal);
				(void)snprintf(msg, mlen,
				    "%s%s(killed by signal %d: %s)", body, sep,
				    s->term_signal,
				    signame != NULL ? signame : "unknown");
			} else {
				(void)snprintf(msg, mlen,
				    "%s%s(exit status %lld)", body, sep,
				    (long long)s->exit_status);
			}
			/* A nonzero exit is an answer, not a broken tool:
			 * grep(1), test(1), and diff(1) all report findings
			 * that way. Reporting it as a failure makes the model
			 * retry a command that already answered. */
			clm_tool_complete(inv, msg);
		} else {
			clm_tool_fail(inv, "out of memory");
		}
	} else {
		clm_tool_complete(
		    inv, s->len ? s->buf : "(command produced no output)");
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

	if (--s->handles > 0) {
		/*
		 * Every real handle (proc + pipes) is closed and the only
		 * thing left open is the still-pending kill_timer (see
		 * shell_cancel): the child went away on its own well within
		 * the grace period, so there's nothing left for the timer to
		 * escalate against. Stop waiting on it and close it now
		 * instead of sitting idle for the rest of
		 * CLM_SHELL_KILL_GRACE_MS
		 * -- shell_kill_timer_cb/shell_force_close handle the timer
		 * actually firing instead.
		 */
		if (s->handles == 1 && s->kill_timer_armed &&
		    !uv_is_closing((uv_handle_t *)&s->kill_timer)) {
			uv_timer_stop(&s->kill_timer);
			uv_close((uv_handle_t *)&s->kill_timer, shell_on_close);
		}
		return;
	}
	shell_finish(s);
}

static void
shell_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct shell_state *s = stream->data;
	if (nread > 0)
		shell_append(s, buf->base, (size_t)nread);
	else if (nread < 0 && !uv_is_closing((uv_handle_t *)stream))
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

/*
 * Force-close any pipe/process handles that are still open, bypassing the
 * normal "wait for the child to actually exit / close its end" path. Each
 * uv_close() here still runs through shell_on_close, so `handles` reaches
 * zero and shell_finish() fires exactly as it would on a clean exit -- this
 * just stops waiting on a child that may never cooperate.
 */
static void
shell_force_close(struct shell_state *s)
{
	if (!uv_is_closing((uv_handle_t *)&s->proc))
		uv_close((uv_handle_t *)&s->proc, shell_on_close);
	if (!uv_is_closing((uv_handle_t *)&s->out))
		uv_close((uv_handle_t *)&s->out, shell_on_close);
	if (!uv_is_closing((uv_handle_t *)&s->err))
		uv_close((uv_handle_t *)&s->err, shell_on_close);
	if (s->has_stdin && !uv_is_closing((uv_handle_t *)&s->in))
		uv_close((uv_handle_t *)&s->in, shell_on_close);
}

static void
shell_kill_timer_cb(uv_timer_t *t)
{
	struct shell_state *s = t->data;

	/* SIGTERM's grace period expired and the process (or a backgrounded
	 * grandchild still holding a pipe open, per the comment in
	 * shell_cancel below) is still around: escalate to SIGKILL for good
	 * measure, then stop waiting for cooperation entirely and force our
	 * own handles closed so shell_finish() is guaranteed to run. */
	kill(-(pid_t)s->proc.pid, SIGKILL);
	shell_force_close(s);
	uv_close((uv_handle_t *)&s->kill_timer, shell_on_close);
}

static void
shell_cancel(struct clm_tool_invocation *inv, void *user)
{
	struct shell_state *s = user;
	(void)inv;
	/*
	 * Signal the whole process group, not just the immediate $SHELL -c
	 * child: a command that backgrounds a long-running job ("cmd &")
	 * leaves that job behind as a sibling in the same group once the
	 * shell itself exits. uv_process_kill() only reaches s->proc's own
	 * pid, so a killed shell whose backgrounded grandchild still holds
	 * the stdout/stderr pipes open never delivers EOF -- shell_read()
	 * never sees nread < 0, the pipes never close, shell_finish() never
	 * runs, and the tool call would hang forever even after cancel
	 * reports success. opt.flags | UV_PROCESS_DETACHED (see
	 * tool_shell_exec) puts the child in its own new process group via
	 * setsid(), so killing -pid here reaches that job too.
	 *
	 * Belt and suspenders: arm a grace-period timer that escalates to
	 * SIGKILL and then force-closes our own pipe handles regardless of
	 * whether the child ever exits. This is what actually bounds the
	 * hang -- SIGTERM/SIGKILL delivery and the child's cooperation are
	 * both best-effort, but shell_force_close() is not.
	 */
	kill(-(pid_t)s->proc.pid, SIGTERM);

	if (!s->kill_timer_armed) {
		uv_loop_t *loop = uv_handle_get_loop((uv_handle_t *)&s->proc);
		s->kill_timer_armed = true;
		s->handles++;
		s->kill_timer.data = s;
		uv_timer_init(loop, &s->kill_timer);
		uv_timer_start(&s->kill_timer, shell_kill_timer_cb,
		    shell_kill_grace_ms(), 0);
	}
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
	json_cleanup cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));
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
	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = sh_arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(
		    inv, "missing required string argument 'command'");
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
	/* New process group (via setsid()) so shell_cancel()'s group kill
	 * also reaches anything the command backgrounded with "&", not just
	 * the $SHELL -c process itself. */
	opt.flags = UV_PROCESS_DETACHED;
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
		uv_buf_t b =
		    uv_buf_init(s->in_buf, (unsigned)strlen(s->in_buf));
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
	        "\"description\":\"optional: data to write to the command's "
	        "standard input\"}},"
	        "\"required\":[\"command\"]}",
	    .invoke = tool_shell_exec,
	    .timeout_ms = CLM_SHELL_DEFAULT_TIMEOUT_MS,
	    .flags =
	        CLM_TOOL_TIMEOUT_OVERRIDABLE | CLM_TOOL_OUTPUT_CAP_OVERRIDABLE,
	};
	return clm_tool_add(agent, &shell_def);
}
