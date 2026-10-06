// SPDX-License-Identifier: ISC
/*
 * shell_exec builtin: run $SHELL -c <command> and answer with its output.
 * A child process needs libuv, so it lives here, not in the core; the
 * process itself is libclmproc's. Register with clm_tools_register_shell().
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/host_uv.h"
#include "clm/cleanup.h"
#include "proc.h"
#include "banned.h"

#define CLM_SHELL_DEFAULT_TIMEOUT_MS 30000u

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

/* The process has ended and its pipes are closed: answer the call. */
static void
shell_done(struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct clm_tool_invocation *inv = user;
	autofree char *out = NULL;
	const char *body = r->output;
	size_t len = r->len;

	(void)p;
	if (r->dropped > 0) {
		if (asprintf(&out,
		        "%s%s[%zu more bytes of output were not kept]\n",
		        r->output,
		        (len && r->output[len - 1] != '\n') ? "\n" : "",
		        r->dropped) < 0)
			out = NULL;
		if (out != NULL) {
			body = out;
			len = strlen(out);
		}
	}

	if (r->exit_status != 0 || r->term_signal != 0) {
		autofree char *msg = NULL;
		/* Command output usually ends in its own newline; do not add
		 * a second one before the status line. */
		const char *sep = (len && body[len - 1] == '\n') ? "" : "\n";
		int n;

		if (len == 0)
			body = "(no output)";
		/* A signal-terminated process has no real exit status: print
		 * the signal instead of a meaningless "exit status 0". */
		if (r->term_signal != 0) {
			const char *signame = strsignal(r->term_signal);

			n = asprintf(&msg, "%s%s(killed by signal %d: %s)",
			    body, sep, r->term_signal,
			    signame != NULL ? signame : "unknown");
		} else {
			n = asprintf(&msg, "%s%s(exit status %lld)", body, sep,
			    (long long)r->exit_status);
		}
		/* A nonzero exit is an answer, not a broken tool: grep(1),
		 * test(1) and diff(1) all report findings that way. Reporting
		 * it as a failure makes the model retry a command that already
		 * answered. */
		if (n < 0)
			clm_tool_fail(inv, "out of memory");
		else
			clm_tool_complete(inv, msg);
		return;
	}
	clm_tool_complete(inv, len ? body : "(command produced no output)");
}

static void
shell_cancel(struct clm_tool_invocation *inv, void *user)
{
	(void)inv;
	clm_proc_cancel(user);
}

static void
tool_shell_exec(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup cJSON *args = cJSON_Parse(clm_tool_invocation_args(inv));
	autofree char *command = NULL;
	autofree char *in = NULL;
	struct clm_proc *p;
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
	in = sh_arg_string(args, "stdin");

	/* Keep up to CLM_TOOL_SPOOL_MAX bytes, more than the model sees, so
	 * the cut can keep the tail and the spool can hold the whole output.
	 * The core's tool timeout stops the command through shell_cancel. */
	struct clm_proc_opts o = {
	    .command = command,
	    .stdin_data = in,
	    .keep = CLM_PROC_KEEP_HEAD,
	    .max = (size_t)CLM_TOOL_SPOOL_MAX,
	    .done = shell_done,
	    .user = inv,
	};
	r = clm_proc_spawn(clm_tool_invocation_loop(inv), &o, &p);
	if (r < 0) {
		clm_tool_fail(inv, uv_strerror(r));
		return;
	}
	clm_tool_invocation_set_cancel(inv, shell_cancel, p);
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
	    .flags = CLM_TOOL_TIMEOUT_OVERRIDABLE,
	};
	return clm_tool_add(agent, &shell_def);
}
