// SPDX-License-Identifier: ISC
/*
 * monitor_start builtin -- spawn a long-running command and turn each line
 * it writes to standard output into a fresh turn via clm_agent_notify(),
 * while it keeps running. bg_exec answers once, when its command exits; a
 * monitor answers repeatedly and never needs the command to exit at all.
 */

/*
 * The source is anything that emits lines: tail -f, inotifywait, a poll
 * loop, "hive listen". The command must flush per line (stdbuf -oL, grep
 * --line-buffered) or its output sits in libc's buffer unseen.
 */

/*
 * Lines that arrive together are coalesced into one notify, so a multi-line
 * event stays one message. Standard error is read and dropped: a monitor's
 * events are its stdout, and a chatty stderr would otherwise fill the pipe
 * and stall the child. A job is capped and stopped if it floods.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/queue.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/host_uv.h"
#include "clm/tools.h"
#include "banned.h"

/* Lines arriving within this window become one notify. */
#define CLM_MON_COALESCE_MS 200

/* A monitor that outruns this in one window is stopped, not throttled: a
 * firehose is a mistake in the command, and silently dropping its events
 * would hide that. */
#define CLM_MON_WINDOW_MS 60000
#define CLM_MON_MAX_EVENTS 60

/* Grace after the stopping SIGTERM before SIGKILL, as tool_shell.c does.
 * The process handle stays open until the child actually exits, so libuv
 * reaps it; closing it earlier would leave a zombie behind. */
#define CLM_MON_KILL_GRACE_MS 5000

#define CLM_MON_LINE_CAP 2048
#define CLM_MON_BATCH_CAP (8 * 1024)
#define CLM_MON_TIMEOUT_MAX 1800000

struct clm_mon_job {
	uint64_t id;
	struct clm_agent *agent; /* where events are delivered */
	char *label;             /* "description" arg, or the command */

	uv_process_t proc;
	uv_pipe_t out, err;
	uv_timer_t deadline, coalesce;

	char *line; /* partial trailing line, no newline seen yet */
	size_t line_len, line_cap;
	char *batch; /* whole lines waiting for the coalesce timer */
	size_t batch_len, batch_cap;
	size_t errbytes; /* stderr is counted, not delivered */

	uint64_t window; /* uv_now when the rate window opened */
	unsigned events;

	int handles; /* proc + out + err + 2 timers; frees itself at 0 */
	bool started;
	bool stopping;
	const char *reason; /* why it stopped, for the closing notify */

	TAILQ_ENTRY(clm_mon_job) entries;
};

TAILQ_HEAD(clm_mon_job_list, clm_mon_job);
static struct clm_mon_job_list mon_jobs = TAILQ_HEAD_INITIALIZER(mon_jobs);
static uint64_t mon_next_id = 1;

static void mon_stop(struct clm_mon_job *j, const char *reason);

/* Local copy of the core's arg_string helper, kept private so libclmuv
 * depends only on libclm's public API; tool_bg.c has its own for the same
 * reason. */
static char *
mon_arg_string(cJSON *args, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
	if (v == NULL || !cJSON_IsString(v))
		return NULL;
	return strdup(cJSON_GetStringValue(v));
}

static struct clm_mon_job *
mon_find(uint64_t id)
{
	struct clm_mon_job *j;

	TAILQ_FOREACH(j, &mon_jobs, entries)
	{
		if (j->id == id)
			return j;
	}
	return NULL;
}

static void
mon_detach(void *user)
{
	struct clm_agent *agent = user;
	struct clm_mon_job *j;

	/* Unlike a background job, a monitor with no agent has nothing left
	 * to deliver to, so it is stopped rather than left running. The list
	 * is safe to walk: mon_stop only closes handles, and the removal
	 * happens later, in the close callback. */
	TAILQ_FOREACH(j, &mon_jobs, entries)
	{
		if (j->agent == agent) {
			j->agent = NULL;
			mon_stop(j, "agent detached");
		}
	}
}

static bool
mon_grow(char **buf, size_t *cap, size_t need)
{
	size_t nc = *cap ? *cap : 1024;
	char *p;

	if (need + 1 <= *cap)
		return true;
	while (nc < need + 1)
		nc *= 2;
	p = realloc(*buf, nc);
	if (p == NULL)
		return false;
	*buf = p;
	*cap = nc;
	return true;
}

/* Hands the batched lines to the agent as one message. */
static void
mon_flush(struct clm_mon_job *j)
{
	autofree char *msg = NULL;
	uint64_t now;

	if (j->batch_len == 0 || j->agent == NULL)
		return;

	now = uv_now(j->coalesce.loop);
	if (now - j->window >= CLM_MON_WINDOW_MS) {
		j->window = now;
		j->events = 0;
	}
	j->events++;

	if (asprintf(&msg, "[monitor %llu (\"%s\")]\n%s",
	        (unsigned long long)j->id, j->label, j->batch) < 0)
		msg = NULL;
	j->batch_len = 0;

	/* On OOM building msg the event is dropped: there is no pending
	 * tool_call_id to report through, and the monitor itself is fine. */
	if (msg != NULL)
		(void)clm_agent_notify(j->agent, msg);

	if (j->events > CLM_MON_MAX_EVENTS)
		mon_stop(j, "too many events");
}

static void
mon_on_coalesce(uv_timer_t *timer)
{
	mon_flush(timer->data);
}

static void
mon_on_deadline(uv_timer_t *timer)
{
	mon_stop(timer->data, "expired");
}

/* Queues one complete line and arms the coalesce timer. */
static void
mon_line(struct clm_mon_job *j, const char *s, size_t n)
{
	if (n > CLM_MON_LINE_CAP)
		n = CLM_MON_LINE_CAP;
	if (j->batch_len + n + 1 > CLM_MON_BATCH_CAP)
		return;
	if (!mon_grow(&j->batch, &j->batch_cap, j->batch_len + n + 1))
		return;

	memcpy(j->batch + j->batch_len, s, n);
	j->batch_len += n;
	j->batch[j->batch_len++] = '\n';
	j->batch[j->batch_len] = '\0';

	uv_timer_start(&j->coalesce, mon_on_coalesce, CLM_MON_COALESCE_MS, 0);
}

static void
mon_feed(struct clm_mon_job *j, const char *data, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (data[i] == '\n') {
			mon_line(
			    j, j->line != NULL ? j->line : "", j->line_len);
			j->line_len = 0;
			continue;
		}
		if (j->line_len >= CLM_MON_LINE_CAP)
			continue;
		if (!mon_grow(&j->line, &j->line_cap, j->line_len + 1))
			return;
		j->line[j->line_len++] = data[i];
		j->line[j->line_len] = '\0';
	}
}

static void
mon_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static void
mon_finish(struct clm_mon_job *j)
{
	if (j->started && j->agent != NULL) {
		autofree char *msg = NULL;
		char errnote[64] = "";

		if (j->errbytes > 0)
			(void)snprintf(errnote, sizeof(errnote),
			    ", %zu bytes on stderr", j->errbytes);

		if (asprintf(&msg, "[monitor %llu (\"%s\") stopped: %s%s]",
		        (unsigned long long)j->id, j->label,
		        j->reason != NULL ? j->reason : "source ended",
		        errnote) < 0)
			msg = NULL;
		if (msg != NULL)
			(void)clm_agent_notify(j->agent, msg);
	}

	TAILQ_REMOVE(&mon_jobs, j, entries);
	free(j->line);
	free(j->batch);
	free(j->label);
	free(j);
}

static void
mon_on_close(uv_handle_t *handle)
{
	struct clm_mon_job *j = handle->data;

	if (--j->handles == 0)
		mon_finish(j);
}

static void
mon_shut(uv_handle_t *h)
{
	if (!uv_is_closing(h))
		uv_close(h, mon_on_close);
}

/* Escalation for a child that ignores SIGTERM: SIGKILL cannot be, so the
 * exit callback always runs and the process handle always closes. */
static void
mon_on_kill(uv_timer_t *timer)
{
	struct clm_mon_job *j = timer->data;

	(void)uv_kill(-uv_process_get_pid(&j->proc), SIGKILL);
	mon_shut((uv_handle_t *)&j->deadline);
}

static void
mon_stop(struct clm_mon_job *j, const char *reason)
{
	if (j->stopping)
		return;
	j->stopping = true;
	j->reason = reason;

	/* Whatever was already buffered is still worth delivering. */
	uv_timer_stop(&j->coalesce);
	mon_flush(j);

	mon_shut((uv_handle_t *)&j->out);
	mon_shut((uv_handle_t *)&j->err);
	mon_shut((uv_handle_t *)&j->coalesce);

	if (!j->started) {
		mon_shut((uv_handle_t *)&j->deadline);
		mon_shut((uv_handle_t *)&j->proc);
		return;
	}

	/* UV_PROCESS_DETACHED put the command in its own process group, so
	 * a group kill reaches anything it backgrounded with "&" too. The
	 * process handle is left open for mon_on_exit to close. */
	(void)uv_kill(-uv_process_get_pid(&j->proc), SIGTERM);
	uv_timer_start(&j->deadline, mon_on_kill, CLM_MON_KILL_GRACE_MS, 0);
}

static void
mon_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct clm_mon_job *j = stream->data;

	if (nread > 0) {
		if (stream == (uv_stream_t *)&j->err)
			j->errbytes += (size_t)nread;
		else
			mon_feed(j, buf->base, (size_t)nread);
	} else if (nread < 0) {
		/* Only standard output ending means the source is done.
		 * Stopping on the other two would race it: a child that
		 * writes nothing to stderr closes that pipe, and exits, with
		 * output still unread in this one. */
		if (stream == (uv_stream_t *)&j->out) {
			if (j->line_len > 0) {
				mon_line(j, j->line, j->line_len);
				j->line_len = 0;
			}
			mon_stop(j, "source ended");
		} else {
			mon_shut((uv_handle_t *)stream);
		}
	}
	free(buf->base);
}

/* Exiting is not the end of the source: output written before the exit may
 * still be sitting in the pipe. The stdout EOF is what ends the monitor. */
static void
mon_on_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	(void)exit_status;
	(void)term_signal;
	struct clm_mon_job *j = proc->data;

	uv_timer_stop(&j->deadline);
	if (j->stopping)
		mon_shut((uv_handle_t *)&j->deadline);
	mon_shut((uv_handle_t *)proc);
}

static void
tool_monitor_start(struct clm_tool_invocation *inv, void *user)
{
	struct clm_agent *agent = user;
	json_cleanup cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));
	autofree char *command = NULL;
	autofree char *description = NULL;
	autofree char *started_msg = NULL;
	struct clm_mon_job *j;
	uv_loop_t *loop = clm_tool_invocation_loop(inv);
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	cJSON *timeout;
	uint64_t ms = CLM_MON_TIMEOUT_MAX;
	const char *shell;
	char *argv[4];
	int r;

	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = mon_arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(
		    inv, "missing required string argument 'command'");
		return;
	}
	description = mon_arg_string(args, "description");

	timeout = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
	if (cJSON_IsNumber(timeout) && cJSON_GetNumberValue(timeout) > 0) {
		double v = cJSON_GetNumberValue(timeout);

		ms = v > (double)CLM_MON_TIMEOUT_MAX ? CLM_MON_TIMEOUT_MAX
		                                     : (uint64_t)v;
	}

	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	j->agent = agent;
	j->id = mon_next_id++;
	j->label = strdup(description != NULL ? description : command);
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
	uv_timer_init(loop, &j->deadline);
	j->deadline.data = j;
	uv_timer_init(loop, &j->coalesce);
	j->coalesce.data = j;
	j->window = uv_now(loop);

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
	opt.exit_cb = mon_on_exit;
	/* New session, as tool_bg.c and tool_shell.c do: no controlling
	 * terminal for the child, and its own process group for the kill in
	 * mon_stop. */
	opt.flags = UV_PROCESS_DETACHED;
	stdio[0].flags = UV_IGNORE;
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&j->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&j->err;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	/* Insert before spawning: the close callbacks below run whether or
	 * not uv_spawn succeeds, and mon_finish always does a TAILQ_REMOVE.
	 * j->started is what keeps a spawn failure, already reported through
	 * clm_tool_fail, from being notified a second time. */
	TAILQ_INSERT_TAIL(&mon_jobs, j, entries);

	j->handles = 5;
	r = uv_spawn(loop, &j->proc, &opt);
	if (r < 0) {
		j->stopping = true;
		mon_shut((uv_handle_t *)&j->proc);
		mon_shut((uv_handle_t *)&j->out);
		mon_shut((uv_handle_t *)&j->err);
		mon_shut((uv_handle_t *)&j->deadline);
		mon_shut((uv_handle_t *)&j->coalesce);
		clm_tool_fail(inv, uv_strerror(r));
		return;
	}
	j->started = true;

	uv_read_start((uv_stream_t *)&j->out, mon_alloc, mon_read);
	uv_read_start((uv_stream_t *)&j->err, mon_alloc, mon_read);
	uv_timer_start(&j->deadline, mon_on_deadline, ms, 0);

	if (asprintf(&started_msg,
	        "started monitor %llu: %s (each line of its output arrives "
	        "later as a separate message, not as this call's result; it "
	        "stops by itself after %llu ms)",
	        (unsigned long long)j->id, j->label,
	        (unsigned long long)ms) < 0)
		started_msg = NULL;
	clm_tool_complete(
	    inv, started_msg != NULL ? started_msg : "started monitor");
}

static void
tool_monitor_stop(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));
	struct clm_mon_job *j;
	cJSON *id;

	(void)user;
	id = cJSON_GetObjectItemCaseSensitive(args, "id");
	if (args == NULL || !cJSON_IsNumber(id)) {
		clm_tool_fail(inv, "missing required number argument 'id'");
		return;
	}

	j = mon_find((uint64_t)cJSON_GetNumberValue(id));
	if (j == NULL) {
		clm_tool_fail(inv, "no such monitor");
		return;
	}
	mon_stop(j, "stopped on request");
	clm_tool_complete(inv, "monitor stopped");
}

static void
tool_monitor_list(struct clm_tool_invocation *inv, void *user)
{
	autofree char *text = NULL;
	size_t cap = 0, len = 0;
	struct clm_mon_job *j;

	(void)user;
	TAILQ_FOREACH(j, &mon_jobs, entries)
	{
		char row[256];
		int n = snprintf(row, sizeof(row), "%llu\t%s\n",
		    (unsigned long long)j->id, j->label);

		if (n < 0 || !mon_grow(&text, &cap, len + (size_t)n))
			break;
		memcpy(text + len, row, (size_t)n);
		len += (size_t)n;
		text[len] = '\0';
	}
	clm_tool_complete(inv, len > 0 ? text : "no monitors running");
}

int
clm_tools_register_monitor(struct clm_agent *agent)
{
	const struct clm_tool_def start_def = {
	    .name = "monitor_start",
	    .description =
	        "watch a long-running command and receive each line it "
	        "prints as a separate message, while it keeps running. Use "
	        "it for a source of events over time -- a log tail, a file "
	        "watch, a poll loop, an agent message bus -- where bg_exec "
	        "would answer only once, when the command exits. The command "
	        "must flush each line as it writes it (stdbuf -oL, grep "
	        "--line-buffered); anything on its standard error is dropped.",
	    .params_schema =
	        "{\"type\":\"object\","
	        "\"properties\":{"
	        "\"command\":{\"type\":\"string\","
	        "\"description\":\"the shell command to watch; every line it "
	        "prints becomes a message\"},"
	        "\"description\":{\"type\":\"string\","
	        "\"description\":\"optional short label naming what is being "
	        "watched; it appears on every message\"},"
	        "\"timeout_ms\":{\"type\":\"number\","
	        "\"description\":\"stop after this long; capped at 1800000, "
	        "which is also the default\"}},"
	        "\"required\":[\"command\"]}",
	    .invoke = tool_monitor_start,
	    .detach = mon_detach,
	    .user = agent,
	};
	const struct clm_tool_def stop_def = {
	    .name = "monitor_stop",
	    .description = "stop a monitor started by monitor_start.",
	    .params_schema = "{\"type\":\"object\","
	                     "\"properties\":{"
	                     "\"id\":{\"type\":\"number\","
	                     "\"description\":\"the monitor id\"}},"
	                     "\"required\":[\"id\"]}",
	    .invoke = tool_monitor_stop,
	    .user = agent,
	};
	const struct clm_tool_def list_def = {
	    .name = "monitor_list",
	    .description = "list the monitors that are still running.",
	    .params_schema = "{\"type\":\"object\",\"properties\":{}}",
	    .invoke = tool_monitor_list,
	    .user = agent,
	};
	int r;

	r = clm_tool_add(agent, &start_def);
	if (r != 0)
		return r;
	r = clm_tool_add(agent, &stop_def);
	if (r != 0)
		return r;
	return clm_tool_add(agent, &list_def);
}
