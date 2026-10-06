// SPDX-License-Identifier: ISC
/* clm_proc: a shell command as a child process on a libuv loop. See proc.h. */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <uv.h>

#include "proc.h"

#define PROC_MAX_DEFAULT ((size_t)1024 * 1024)
#define PROC_GRACE_DEFAULT 5000u

struct clm_proc {
	uv_process_t proc;
	uv_pipe_t out, err, in;
	uv_timer_t timer; /* the timeout, then the grace before SIGKILL */
	uv_write_t wreq;
	int handles; /* still open; done runs when this reaches 0 */
	int pipes;   /* out and err still open */
	bool has_stdin, exited, cancelled, timed_out, failed;
	char *in_buf;
	char *buf;
	size_t len, bufcap, max, head, dropped;
	enum clm_proc_keep keep;
	int64_t exit_status;
	int term_signal;
	uint64_t exit_grace_ms;
	clm_proc_output_fn output;
	clm_proc_done_fn done;
	void *user;
};

uint64_t
clm_proc_grace_ms(void)
{
	static uint64_t ms;
	static bool init;
	const char *e;

	if (init)
		return ms;
	init = true;
	ms = PROC_GRACE_DEFAULT;
	e = getenv("CLM_SHELL_KILL_GRACE_MS");
	if (e != NULL) {
		char *end;
		unsigned long long v = strtoull(e, &end, 10);

		if (end != e && *end == '\0')
			ms = (uint64_t)v;
	}
	return ms;
}

static bool
grow(struct clm_proc *p, size_t need)
{
	size_t nc = p->bufcap ? p->bufcap : 4096;
	char *b;

	if (need + 1 <= p->bufcap)
		return true;
	while (nc < need + 1)
		nc *= 2;
	if (nc > p->max + 1)
		nc = p->max + 1;
	b = realloc(p->buf, nc);
	if (b == NULL)
		return false;
	p->buf = b;
	p->bufcap = nc;
	return true;
}

static void
keep_head(struct clm_proc *p, const char *data, size_t n)
{
	size_t take = p->len >= p->max ? 0 : p->max - p->len;

	if (take > n)
		take = n;
	p->dropped += n - take;
	if (take == 0)
		return;
	if (!grow(p, p->len + take)) {
		p->dropped += take;
		return;
	}
	memcpy(p->buf + p->len, data, take);
	p->len += take;
	p->buf[p->len] = '\0';
}

/* The first head bytes stay; past max the oldest bytes after them go, so
 * the newest output always survives. */
static void
keep_ends(struct clm_proc *p, const char *data, size_t n)
{
	size_t room = p->max - p->head;
	size_t want;

	/* The start goes to the head region first, whatever the chunk size. */
	if (p->len < p->head) {
		size_t take = p->head - p->len < n ? p->head - p->len : n;

		keep_head(p, data, take);
		data += take;
		n -= take;
		if (n == 0)
			return;
	}
	if (n > room) {
		p->dropped += n - room;
		data += n - room;
		n = room;
	}
	want = p->len + n < p->max ? p->len + n : p->max;
	if (!grow(p, want)) {
		p->dropped += n;
		return;
	}
	if (p->len + n > p->max) {
		size_t evict = p->len + n - p->max;

		memmove(p->buf + p->head, p->buf + p->head + evict,
		    p->len - p->head - evict);
		p->len -= evict;
		p->dropped += evict;
	}
	memcpy(p->buf + p->len, data, n);
	p->len += n;
	p->buf[p->len] = '\0';
}

static void
finish(struct clm_proc *p)
{
	if (!p->failed && p->done != NULL) {
		struct clm_proc_result r = {
		    .exit_status = p->exit_status,
		    .term_signal = p->term_signal,
		    .timed_out = p->timed_out,
		    .cancelled = p->cancelled,
		    .output = p->buf != NULL ? p->buf : "",
		    .len = p->len,
		    .dropped = p->dropped,
		};

		p->done(p, &r, p->user);
	}
	free(p->in_buf);
	free(p->buf);
	free(p);
}

static void
on_closed(uv_handle_t *h)
{
	struct clm_proc *p = h->data;

	if (--p->handles == 0) {
		finish(p);
		return;
	}
	/* Only the timer is left, and nothing remains for it to guard. */
	if (p->handles == 1 && !uv_is_closing((uv_handle_t *)&p->timer)) {
		uv_timer_stop(&p->timer);
		uv_close((uv_handle_t *)&p->timer, on_closed);
	}
}

static void
shut(uv_handle_t *h)
{
	if (!uv_is_closing(h))
		uv_close(h, on_closed);
}

static void
shut_pipe(struct clm_proc *p, uv_pipe_t *s)
{
	if (uv_is_closing((uv_handle_t *)s))
		return;
	p->pipes--;
	uv_close((uv_handle_t *)s, on_closed);
}

static void
close_pipes(struct clm_proc *p)
{
	shut_pipe(p, &p->out);
	shut_pipe(p, &p->err);
	if (p->has_stdin)
		shut((uv_handle_t *)&p->in);
}

/*
 * The group id is the shell's pid. After the shell is reaped it stays the
 * group's only while a member lives, and an open pipe is the sign of one;
 * without that sign the id may belong to someone else, so it is left alone.
 */
static void
signal_group(struct clm_proc *p, int sig)
{
	if (!p->exited || p->pipes > 0)
		(void)kill(-(pid_t)p->proc.pid, sig);
}

static void
on_kill(uv_timer_t *t)
{
	struct clm_proc *p = t->data;

	signal_group(p, SIGKILL);
	close_pipes(p);
	shut((uv_handle_t *)t);
}

static void
on_timeout(uv_timer_t *t)
{
	struct clm_proc *p = t->data;

	p->timed_out = true;
	clm_proc_cancel(p);
}

void
clm_proc_cancel(struct clm_proc *p)
{
	if (p->cancelled)
		return;
	p->cancelled = true;
	signal_group(p, SIGTERM);
	if (!uv_is_closing((uv_handle_t *)&p->timer))
		uv_timer_start(&p->timer, on_kill, clm_proc_grace_ms(), 0);
}

void
clm_proc_stop_output(struct clm_proc *p)
{
	/* Signal before the pipes close: an exited shell's group is only
	 * known to be alive while one is open. */
	if (!p->cancelled)
		signal_group(p, SIGTERM);
	close_pipes(p);
	clm_proc_cancel(p);
}

void
clm_proc_kill(struct clm_proc *p)
{
	p->cancelled = true;
	signal_group(p, SIGKILL);
	close_pipes(p);
}

void
clm_proc_detach(struct clm_proc *p)
{
	p->done = NULL;
	p->output = NULL;
}

static void
on_exited(uv_process_t *pr, int64_t status, int sig)
{
	struct clm_proc *p = pr->data;

	p->exited = true;
	p->exit_status = status;
	p->term_signal = sig;
	shut((uv_handle_t *)pr);
	if (p->cancelled)
		return;
	if (p->pipes > 0 && p->exit_grace_ms > 0)
		uv_timer_start(&p->timer, on_kill, p->exit_grace_ms, 0);
	else
		uv_timer_stop(&p->timer);
}

static void
on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
	(void)h;
	buf->base = malloc(suggested);
	buf->len = buf->base != NULL ? suggested : 0;
}

static void
on_read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf)
{
	struct clm_proc *p = s->data;
	int fd = s == (uv_stream_t *)&p->out ? 1 : 2;

	if (nread > 0) {
		if (p->output != NULL)
			p->output(p, fd, buf->base, (size_t)nread, p->user);
		else if (p->keep == CLM_PROC_KEEP_ENDS)
			keep_ends(p, buf->base, (size_t)nread);
		else
			keep_head(p, buf->base, (size_t)nread);
	} else if (nread < 0) {
		if (p->output != NULL)
			p->output(p, fd, NULL, 0, p->user);
		shut_pipe(p, (uv_pipe_t *)s);
	}
	free(buf->base);
}

static void
on_written(uv_write_t *req, int status)
{
	struct clm_proc *p = req->data;

	(void)status;
	shut((uv_handle_t *)&p->in);
}

int
clm_proc_spawn(
    uv_loop_t *loop, const struct clm_proc_opts *o, struct clm_proc **out)
{
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	struct clm_proc *p;
	const char *shell = o->shell;
	char *argv[4];
	int r;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return UV_ENOMEM;
	if (o->stdin_data != NULL) {
		p->in_buf = strdup(o->stdin_data);
		if (p->in_buf == NULL) {
			free(p);
			return UV_ENOMEM;
		}
	}
	p->has_stdin = p->in_buf != NULL;
	p->keep = o->keep;
	p->max = o->max > 0 ? o->max : PROC_MAX_DEFAULT;
	p->head = o->head < p->max ? o->head : p->max / 4;
	p->exit_grace_ms = o->exit_grace_ms;
	p->output = o->output;
	p->done = o->done;
	p->user = o->user;
	p->proc.data = p->out.data = p->err.data = p->in.data = p;
	p->timer.data = p->wreq.data = p;
	uv_pipe_init(loop, &p->out, 0);
	uv_pipe_init(loop, &p->err, 0);
	uv_timer_init(loop, &p->timer);
	if (p->has_stdin) {
		uv_pipe_init(loop, &p->in, 0);
		stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
		stdio[0].data.stream = (uv_stream_t *)&p->in;
	} else {
		stdio[0].flags = UV_IGNORE;
	}
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&p->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&p->err;

	if (shell == NULL || shell[0] == '\0')
		shell = getenv("SHELL");
	if (shell == NULL || shell[0] == '\0')
		shell = "/bin/sh";
	argv[0] = (char *)shell;
	argv[1] = "-c";
	argv[2] = (char *)o->command;
	argv[3] = NULL;
	memset(&opt, 0, sizeof(opt));
	opt.file = shell;
	opt.args = argv;
	opt.cwd = o->cwd;
	opt.exit_cb = on_exited;
	/*
	 * A new session: no controlling terminal, so a child that opens
	 * /dev/tty (ssh's host key notice) cannot write over a TUI, and a
	 * process group of its own, so a kill reaches its background jobs.
	 */
	opt.flags = UV_PROCESS_DETACHED;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	p->handles = p->has_stdin ? 5 : 4;
	p->pipes = 2;
	r = uv_spawn(loop, &p->proc, &opt);
	if (r < 0) {
		/* The handles must still be closed; done is not called. */
		p->failed = true;
		shut((uv_handle_t *)&p->proc);
		close_pipes(p);
		shut((uv_handle_t *)&p->timer);
		return r;
	}
	uv_read_start((uv_stream_t *)&p->out, on_alloc, on_read);
	uv_read_start((uv_stream_t *)&p->err, on_alloc, on_read);
	if (o->timeout_ms > 0)
		uv_timer_start(&p->timer, on_timeout, o->timeout_ms, 0);
	if (p->has_stdin) {
		uv_buf_t b =
		    uv_buf_init(p->in_buf, (unsigned)strlen(p->in_buf));

		if (uv_write(
		        &p->wreq, (uv_stream_t *)&p->in, &b, 1, on_written) < 0)
			shut((uv_handle_t *)&p->in);
	}
	*out = p;
	return 0;
}
