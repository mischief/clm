// SPDX-License-Identifier: ISC
/*
 * clm_proc: child processes that always end, report what they wrote, and
 * leave no process behind once the owner asks them to stop.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "proc.h"
#include "tap.h"

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

struct got {
	bool done;
	int64_t status;
	int sig;
	bool timed_out, cancelled;
	char out[4096];
	size_t len, dropped;
	/* For an output callback. */
	char streamed[256];
	size_t nstreamed;
	int eofs;
	bool stop_on_output;
};

static void
on_done(struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct got *g = user;
	size_t n = r->len < sizeof(g->out) - 1 ? r->len : sizeof(g->out) - 1;

	(void)p;
	g->done = true;
	g->status = r->exit_status;
	g->sig = r->term_signal;
	g->timed_out = r->timed_out;
	g->cancelled = r->cancelled;
	memcpy(g->out, r->output, n);
	g->out[n] = '\0';
	g->len = r->len;
	g->dropped = r->dropped;
}

static void
on_output(struct clm_proc *p, int fd, const char *data, size_t n, void *user)
{
	struct got *g = user;

	if (data == NULL) {
		g->eofs++;
		return;
	}
	if (g->nstreamed + n + 3 < sizeof(g->streamed)) {
		g->streamed[g->nstreamed++] = (char)('0' + fd);
		memcpy(g->streamed + g->nstreamed, data, n);
		g->nstreamed += n;
		g->streamed[g->nstreamed] = '\0';
	}
	if (g->stop_on_output) {
		g->stop_on_output = false;
		clm_proc_stop_output(p);
	}
}

/* Run the loop until g is done or ms pass. Returns the time it took. */
static uint64_t
wait_done(uv_loop_t *loop, struct got *g, uint64_t ms)
{
	uint64_t start = uv_hrtime();

	while (!g->done && (uv_hrtime() - start) / 1000000 < ms)
		uv_run(loop, UV_RUN_NOWAIT), uv_sleep(5);
	return (uv_hrtime() - start) / 1000000;
}

static struct clm_proc *
start(uv_loop_t *loop, struct clm_proc_opts *o, struct got *g)
{
	struct clm_proc *p = NULL;

	o->done = on_done;
	o->user = g;
	if (o->shell == NULL)
		o->shell = "/bin/sh";
	CHECK(clm_proc_spawn(loop, o, &p) == 0, "spawn");
	return p;
}

static int
test_exit_and_output(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {.command = "echo out; echo err >&2; exit 3"};

	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(
	    g.done && g.status == 3 && g.sig == 0, "the exit code comes back");
	CHECK(strstr(g.out, "out") && strstr(g.out, "err"),
	    "stdout and stderr are kept together");
	CHECK(!g.cancelled && !g.timed_out,
	    "a command that ends is not cancelled");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){
	    .command = "pwd; cat", .cwd = "/tmp", .stdin_data = "fed"};
	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(g.done && strcmp(g.out, "/tmp\nfed") == 0, "cwd and stdin");
	return 0;
}

static int
test_keep(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {
	    .command = "head -c 5000 /dev/zero | tr '\\0' x", .max = 100};

	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(g.done && g.len == 100 && g.dropped == 4900,
	    "the head is kept and the rest counted");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){
	    .command = "printf HEAD; head -c 5000 /dev/zero | tr '\\0' x; "
	               "printf TAIL",
	    .keep = CLM_PROC_KEEP_ENDS,
	    .max = 100,
	    .head = 10};
	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(g.done && g.len <= 100 && strncmp(g.out, "HEAD", 4) == 0 &&
	        strcmp(g.out + g.len - 4, "TAIL") == 0 &&
	        g.len + g.dropped == 5008,
	    "both ends are kept and the middle counted");
	return 0;
}

static int
test_stopping(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {.command = "sleep 5", .timeout_ms = 100};
	struct clm_proc *p;
	uint64_t took;

	start(loop, &o, &g);
	took = wait_done(loop, &g, 5000);
	CHECK(g.done && g.timed_out && g.cancelled && g.sig == SIGTERM,
	    "a timeout ends it with SIGTERM");
	CHECK(took < 2000, "it ends soon after the timeout");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){
	    .command = "trap '' TERM; sleep 10", .timeout_ms = 100};
	start(loop, &o, &g);
	took = wait_done(loop, &g, 5000);
	CHECK(g.done && g.sig == SIGKILL,
	    "a command that ignores SIGTERM is killed");
	CHECK(took < 2000, "after the grace period");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){.command = "sleep 10"};
	p = start(loop, &o, &g);
	clm_proc_cancel(p);
	wait_done(loop, &g, 5000);
	CHECK(g.done && g.cancelled && !g.timed_out, "cancel ends it");

	memset(&g, 0, sizeof(g));
	p = start(loop, &o, &g);
	clm_proc_kill(p);
	wait_done(loop, &g, 5000);
	CHECK(g.done && g.sig == SIGKILL, "kill ends it at once");
	return 0;
}

static int
test_leftovers(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {
	    .command = "sleep 30 & echo started", .exit_grace_ms = 200};
	struct clm_proc *p;
	uint64_t took;

	start(loop, &o, &g);
	took = wait_done(loop, &g, 5000);
	CHECK(g.done && g.status == 0 && strstr(g.out, "started"),
	    "a job left in the background does not keep it open");
	CHECK(took < 2000, "the job is killed after the exit grace");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){.command = "sleep 30 & echo started"};
	p = start(loop, &o, &g);
	wait_done(loop, &g, 300);
	CHECK(!g.done, "without an exit grace it waits for the pipes");
	clm_proc_cancel(p);
	wait_done(loop, &g, 5000);
	CHECK(g.done, "and a cancel still ends it");
	return 0;
}

static int
test_streaming(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {
	    .command = "echo a; sleep 0.1; echo b >&2", .output = on_output};

	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(
	    g.done && strstr(g.streamed, "1a\n") && strstr(g.streamed, "2b\n"),
	    "output arrives by stream");
	CHECK(g.eofs == 2 && g.len == 0,
	    "each stream ends once; nothing is kept");

	memset(&g, 0, sizeof(g));
	g.stop_on_output = true;
	o = (struct clm_proc_opts){
	    .command = "while :; do echo x; sleep 0.01; done",
	    .output = on_output};
	start(loop, &o, &g);
	wait_done(loop, &g, 5000);
	CHECK(g.done && g.cancelled,
	    "stopping the output from its callback ends it");
	return 0;
}

static int
test_owner_gone(void *arg)
{
	uv_loop_t *loop = arg;
	struct got g = {0};
	struct clm_proc_opts o = {.command = "sleep 0.2"};
	struct clm_proc *p;
	int r;

	p = start(loop, &o, &g);
	clm_proc_detach(p);
	uv_run(loop, UV_RUN_DEFAULT);
	CHECK(!g.done, "a detached process does not call back");

	memset(&g, 0, sizeof(g));
	o = (struct clm_proc_opts){.command = "true",
	    .cwd = "/nonexistent",
	    .shell = "/bin/sh",
	    .done = on_done,
	    .user = &g};
	r = clm_proc_spawn(loop, &o, &p);
	CHECK(r < 0, "a spawn that fails returns its error");
	uv_run(loop, UV_RUN_DEFAULT);
	CHECK(!g.done, "and never calls back");
	return 0;
}

int
main(void)
{
	static uv_loop_t loop;

	/* Short grace, so the SIGKILL path does not take 5 seconds. */
	setenv("CLM_SHELL_KILL_GRACE_MS", "200", 1);
	uv_loop_init(&loop);
	TAP_ADD(
	    "exit code, output, cwd and stdin", test_exit_and_output, &loop);
	TAP_ADD("output past max", test_keep, &loop);
	TAP_ADD("timeout, cancel and kill", test_stopping, &loop);
	TAP_ADD("jobs left in the background", test_leftovers, &loop);
	TAP_ADD("streamed output", test_streaming, &loop);
	TAP_ADD("detach and spawn failure", test_owner_gone, &loop);
	return tap_run();
}
