// SPDX-License-Identifier: ISC
/*
 * monitor_start builtin: each line a long-running command writes to stdout
 * becomes a fresh turn via clm_agent_notify(), while it keeps running.
 * libclmproc runs the process and streams its output here.
 */

/*
 * The command must flush per line (stdbuf -oL, grep --line-buffered).
 * Lines that arrive together become one notify; stderr is counted, not
 * delivered; a monitor that floods is stopped.
 */
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
#include "proc.h"
#include "banned.h"

/* Lines arriving within this window become one notify. */
#define CLM_MON_COALESCE_MS 200

/* A monitor that outruns this in one window is stopped, not throttled: a
 * firehose is a mistake in the command, and silently dropping its events
 * would hide that. */
#define CLM_MON_WINDOW_MS 60000
#define CLM_MON_MAX_EVENTS 60

#define CLM_MON_LINE_CAP 2048
#define CLM_MON_BATCH_CAP (8 * 1024)
#define CLM_MON_TIMEOUT_MAX 1800000

struct clm_mon_job {
	uint64_t id;
	struct clm_agent *agent; /* where events are delivered */
	char *label;             /* "description" arg, or the command */

	struct clm_proc *proc; /* NULL once it has ended */
	uv_timer_t coalesce;

	char *line; /* partial trailing line, no newline seen yet */
	size_t line_len, line_cap;
	char *batch; /* whole lines waiting for the coalesce timer */
	size_t batch_len, batch_cap;
	size_t errbytes; /* stderr is counted, not delivered */

	uint64_t window; /* uv_now when the rate window opened */
	unsigned events;

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

/* Unlike a background job, a monitor with no agent has nothing left to
 * deliver to, so it is stopped rather than left running. */
static void
mon_detach(void *user)
{
	struct clm_agent *agent = user;
	struct clm_mon_job *j;

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

	if (!j->stopping)
		uv_timer_start(
		    &j->coalesce, mon_on_coalesce, CLM_MON_COALESCE_MS, 0);
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
mon_stop(struct clm_mon_job *j, const char *reason)
{
	if (j->stopping)
		return;
	j->stopping = true;
	j->reason = reason;

	/* Whatever was already buffered is still worth delivering. */
	uv_timer_stop(&j->coalesce);
	mon_flush(j);
	if (j->proc != NULL)
		clm_proc_stop_output(j->proc);
}

/* Only standard output ending means the source is done: a child that
 * writes nothing to stderr closes that pipe, and may exit, with output
 * still unread on stdout. */
static void
mon_output(struct clm_proc *p, int fd, const char *data, size_t n, void *user)
{
	struct clm_mon_job *j = user;

	(void)p;
	if (fd == 2) {
		j->errbytes += n;
		return;
	}
	if (data != NULL) {
		mon_feed(j, data, n);
		return;
	}
	if (j->line_len > 0) {
		mon_line(j, j->line, j->line_len);
		j->line_len = 0;
	}
	mon_stop(j, "source ended");
}

static void
mon_freed(uv_handle_t *h)
{
	struct clm_mon_job *j = h->data;

	TAILQ_REMOVE(&mon_jobs, j, entries);
	free(j->line);
	free(j->batch);
	free(j->label);
	free(j);
}

static void
mon_done(struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct clm_mon_job *j = user;

	(void)p;
	j->proc = NULL;
	if (!j->stopping) {
		/* The timeout ended it; deliver what is buffered first. */
		j->stopping = true;
		j->reason = r->timed_out ? "expired" : "source ended";
		mon_flush(j);
	}
	uv_timer_stop(&j->coalesce);
	if (j->agent != NULL) {
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
	uv_close((uv_handle_t *)&j->coalesce, mon_freed);
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
	cJSON *timeout;
	uint64_t ms = CLM_MON_TIMEOUT_MAX;
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
	j->label = strdup(description != NULL ? description : command);
	if (j->label == NULL) {
		free(j);
		clm_tool_fail(inv, "out of memory");
		return;
	}

	struct clm_proc_opts o = {
	    .command = command,
	    .timeout_ms = ms,
	    .output = mon_output,
	    .done = mon_done,
	    .user = j,
	};
	r = clm_proc_spawn(loop, &o, &j->proc);
	if (r < 0) {
		free(j->label);
		free(j);
		clm_tool_fail(inv, uv_strerror(r));
		return;
	}
	j->id = mon_next_id++;
	uv_timer_init(loop, &j->coalesce);
	j->coalesce.data = j;
	j->window = uv_now(loop);
	TAILQ_INSERT_TAIL(&mon_jobs, j, entries);

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
