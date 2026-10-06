// SPDX-License-Identifier: ISC
/*
 * bg_exec builtin: start $SHELL -c <command> and answer at once. The result
 * comes later through clm_agent_notify() as a new turn, since an API takes
 * no late result on an answered tool call. libclmproc runs the process.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <sys/queue.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/host_uv.h"
#include "clm/cleanup.h"
#include "proc.h"
#include "banned.h"

/* Output kept per job for the eventual notify. */
#define CLM_BG_OUTPUT_CAP (16 * 1024)

/*
 * How much of the start of the output survives once the cap is reached. A
 * long-running job says what it is doing at the start and how it went at the
 * end, so keep both ends and drop the middle.
 */
#define CLM_BG_HEAD_KEEP (4 * 1024)

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
	char *label; /* the "label" arg, or the command if none given */
	TAILQ_ENTRY(clm_bg_job) entries;
};

TAILQ_HEAD(clm_bg_job_list, clm_bg_job);
static struct clm_bg_job_list bg_jobs = TAILQ_HEAD_INITIALIZER(bg_jobs);
static uint64_t bg_next_id = 1;

/* The agent is going away: its jobs keep running, and their end only frees
 * them, without a notify to the freed agent. */
static void
bg_detach(void *user)
{
	struct clm_agent *agent = user;
	struct clm_bg_job *j;

	TAILQ_FOREACH(j, &bg_jobs, entries)
	{
		if (j->agent == agent)
			j->agent = NULL;
	}
}

static void
bg_done(struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct clm_bg_job *j = user;

	(void)p;
	if (j->agent != NULL) {
		autofree char *msg = NULL;
		char dropnote[64] = "";
		int n;

		if (r->dropped > 0)
			(void)snprintf(dropnote, sizeof(dropnote),
			    ", %zu bytes dropped from the middle", r->dropped);

		/* A signal-terminated process has no real exit status (see
		 * tool_shell.c's shell_done): report the signal instead. */
		if (r->term_signal != 0) {
			const char *signame = strsignal(r->term_signal);

			n = asprintf(&msg,
			    "[background job %llu (\"%s\") finished, killed by "
			    "signal %d: %s%s]\n%s",
			    (unsigned long long)j->id, j->label, r->term_signal,
			    signame != NULL ? signame : "unknown", dropnote,
			    r->len ? r->output : "(no output)");
		} else {
			n = asprintf(&msg,
			    "[background job %llu (\"%s\") finished, exit "
			    "status "
			    "%lld%s]\n%s",
			    (unsigned long long)j->id, j->label,
			    (long long)r->exit_status, dropnote,
			    r->len ? r->output : "(no output)");
		}
		/* On OOM the notification is dropped: the job itself ran to
		 * completion, and there is no tool call left to report a
		 * failure through (see the file comment). */
		if (n >= 0 && msg != NULL)
			(void)clm_agent_notify(j->agent, msg);
	}

	TAILQ_REMOVE(&bg_jobs, j, entries);
	free(j->label);
	free(j);
}

static void
tool_bg_exec(struct clm_tool_invocation *inv, void *user)
{
	struct clm_agent *agent = user;
	json_cleanup cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));
	autofree char *command = NULL;
	autofree char *label = NULL;
	autofree char *started_msg = NULL;
	struct clm_bg_job *j;
	struct clm_proc *p;
	int r;

	if (args == NULL || !cJSON_IsObject(args)) {
		clm_tool_fail(inv, "invalid arguments");
		return;
	}
	command = bg_arg_string(args, "command");
	if (command == NULL) {
		clm_tool_fail(
		    inv, "missing required string argument 'command'");
		return;
	}
	label = bg_arg_string(args, "label");

	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	j->agent = agent;
	j->label = strdup(label != NULL ? label : command);
	if (j->label == NULL) {
		free(j);
		clm_tool_fail(inv, "out of memory");
		return;
	}

	struct clm_proc_opts o = {
	    .command = command,
	    .keep = CLM_PROC_KEEP_ENDS,
	    .max = (size_t)CLM_BG_OUTPUT_CAP,
	    .head = (size_t)CLM_BG_HEAD_KEEP,
	    .done = bg_done,
	    .user = j,
	};
	r = clm_proc_spawn(clm_tool_invocation_loop(inv), &o, &p);
	if (r < 0) {
		free(j->label);
		free(j);
		clm_tool_fail(inv, uv_strerror(r));
		return;
	}
	j->id = bg_next_id++;
	TAILQ_INSERT_TAIL(&bg_jobs, j, entries);

	if (asprintf(&started_msg,
	        "started background job %llu: %s (result will arrive later as "
	        "a new message, not as this call's result)",
	        (unsigned long long)j->id, j->label) < 0)
		started_msg = NULL;
	clm_tool_complete(
	    inv, started_msg != NULL ? started_msg : "started background job");
}

int
clm_tools_register_bg(struct clm_agent *agent)
{
	const struct clm_tool_def bg_def = {
	    .name = "bg_exec",
	    .description =
	        "start a shell command running in the background and return "
	        "immediately with a job id, instead of waiting for it to "
	        "finish "
	        "(use shell_exec for that). The command's real output arrives "
	        "later as a separate message tagged with the same job id, not "
	        "as this call's result -- do not wait for it, continue with "
	        "other work. Only the first and last few kilobytes of a "
	        "chatty job's output survive; redirect to a file and read "
	        "that instead if you need all of it, or progress before it "
	        "exits.",
	    .params_schema =
	        "{\"type\":\"object\","
	        "\"properties\":{"
	        "\"command\":{\"type\":\"string\","
	        "\"description\":\"the shell command to run in the "
	        "background\"},"
	        "\"label\":{\"type\":\"string\","
	        "\"description\":\"optional short label to identify this job "
	        "in "
	        "its later result message; defaults to the command itself\"}},"
	        "\"required\":[\"command\"]}",
	    .invoke = tool_bg_exec,
	    .detach = bg_detach,
	    .user = agent,
	};
	return clm_tool_add(agent, &bg_def);
}
