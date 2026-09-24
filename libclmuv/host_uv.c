// SPDX-License-Identifier: ISC
/*
 * Desktop clm_host adapter over libcurl + libuv. Implements the transport by
 * delegating to the existing async HTTP engine (http_async.c), and timers via
 * uv_timer. See clm/host_uv.h.
 */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <uv.h>

#include "clm/host.h"
#include "clm/host_uv.h"
#include "clm/http_async.h"
#include "banned.h"

/*
 * host->ctx for this adapter: the loop (needed for timer_set, which has
 * nothing to do with HTTP) plus one clm_http_mux shared by every HTTP
 * request this host ever starts -- the agent's own LLM API calls and every
 * Lua plugin's http.get/post alike, since both already route through
 * agent->host->http_post (see libclmlua/lua_http.c). One host, one mux, for
 * the host's whole lifetime: curl's connection/TLS-session cache is then
 * reused across every request instead of each paying for a fresh handshake.
 *
 * Scoped to one clm_host, not a process-wide global: nothing here prevents
 * a caller from constructing more than one clm_host (and hence more than
 * one clm_agent) in the same process -- each gets its own independent mux,
 * so there is never a question of one host's connection cache leaking into
 * another's, and no shared/static state to reason about across them.
 */
struct host_uv_ctx {
	uv_loop_t *loop;
	struct clm_http_mux *mux;
	/* Every call is tracked, including fire-and-forget health/model probes.
	 * The core intentionally does not retain cancellable handles for those
	 * probes, but the host must still settle them before it destroys mux.
	 */
	struct host_uv_call *calls;
};

struct host_uv_call {
	struct host_uv_ctx *hctx;
	struct host_uv_call *next;
	struct clm_http_request *req;
	clm_http_success_cb success;
	clm_http_error_cb error;
	clm_http_data_cb data;
	void *user;
	bool starting;
	bool completed;
};

static void
host_uv_call_unlink(struct host_uv_call *call)
{
	struct host_uv_call **p;

	for (p = &call->hctx->calls; *p != NULL; p = &(*p)->next) {
		if (*p == call) {
			*p = call->next;
			return;
		}
	}
}

static void
host_uv_http_success(struct clm_http_response *resp, void *user)
{
	struct host_uv_call *call = user;
	clm_http_success_cb success = call->success;
	void *cb_user = call->user;

	host_uv_call_unlink(call);
	call->completed = true;
	if (call->starting) {
		success(resp, cb_user);
		return;
	}
	free(call);
	success(resp, cb_user);
}

static void
host_uv_http_error(int error_code, const char *error_msg, void *user)
{
	struct host_uv_call *call = user;
	clm_http_error_cb error = call->error;
	void *cb_user = call->user;

	host_uv_call_unlink(call);
	call->completed = true;
	if (call->starting) {
		error(error_code, error_msg, cb_user);
		return;
	}
	free(call);
	error(error_code, error_msg, cb_user);
}

static void
host_uv_http_data(const char *data, size_t len, void *user)
{
	struct host_uv_call *call = user;

	if (call->data != NULL)
		call->data(data, len, call->user);
}

/* ------------------------------------------------------------------ */
/* HTTP transport                                                      */
/* ------------------------------------------------------------------ */

static int
host_uv_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error, clm_http_data_cb data,
    void *user, struct clm_http_call **out)
{
	struct host_uv_ctx *hctx = ctx;
	struct host_uv_call *call;
	struct curl_slist *hdrs = NULL;
	struct clm_http_request *r = NULL;
	int rc;

	if (out != NULL)
		*out = NULL;

	call = calloc(1, sizeof(*call));
	if (call == NULL)
		return -ENOMEM;
	call->hctx = hctx;
	call->success = success;
	call->error = error;
	call->data = data;
	call->user = user;
	call->next = hctx->calls;
	hctx->calls = call;
	call->starting = true;

	/* Translate the portable "Name: Value" header list into a curl_slist.
	 * clm_http_async_post takes ownership on success. */
	if (req->headers != NULL) {
		for (const char *const *h = req->headers; *h != NULL; h++) {
			struct curl_slist *n = curl_slist_append(hdrs, *h);
			if (n == NULL) {
				curl_slist_free_all(hdrs);
				host_uv_call_unlink(call);
				free(call);
				return -ENOMEM;
			}
			hdrs = n;
		}
	}

	rc = clm_http_async_post(hctx->mux, req->url, req->api_key, req->body,
	    hdrs, host_uv_http_success, host_uv_http_error,
	    data != NULL ? host_uv_http_data : NULL, req->client_suffix, call,
	    &r);
	if (rc < 0) {
		/* The engine did not take the headers on a start failure. */
		curl_slist_free_all(hdrs);
		host_uv_call_unlink(call);
		free(call);
		return rc;
	}
	call->starting = false;
	if (call->completed) {
		free(call);
		return 0;
	}
	if (r != NULL)
		call->req = r;
	if (out != NULL)
		*out = (struct clm_http_call *)r;
	return 0;
}

static void
host_uv_http_cancel(struct clm_http_call *call)
{
	clm_http_async_cancel((struct clm_http_request *)call);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

/* uv_timer_t is first so (uv_handle_t *) and (struct clm_timer *) casts alias.
 */
struct clm_timer {
	uv_timer_t t;
	clm_timer_cb cb;
	void *arg;
};

static void
host_uv_timer_close_cb(uv_handle_t *h)
{
	free((struct clm_timer *)h);
}

static void
host_uv_timer_fire(uv_timer_t *t)
{
	struct clm_timer *tm = (struct clm_timer *)t;
	/* One-shot: the handle stays valid until the core calls timer_cancel to
	 * release it (mirrors the core's "always tear the timer down"
	 * teardown). */
	tm->cb(tm->arg);
}

static int
host_uv_timer_set(
    void *ctx, uint64_t ms, clm_timer_cb cb, void *arg, struct clm_timer **out)
{
	struct host_uv_ctx *hctx = ctx;
	struct clm_timer *tm = calloc(1, sizeof(*tm));
	if (tm == NULL)
		return -ENOMEM;
	tm->cb = cb;
	tm->arg = arg;
	uv_timer_init(hctx->loop, &tm->t);
	uv_timer_start(&tm->t, host_uv_timer_fire, ms, 0);
	if (out != NULL)
		*out = tm;
	return 0;
}

static void
host_uv_timer_cancel(struct clm_timer *tm)
{
	if (tm == NULL)
		return;
	uv_timer_stop(&tm->t);
	uv_close((uv_handle_t *)&tm->t, host_uv_timer_close_cb);
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

/* Grace after the SIGTERM of proc_detach before SIGKILL. */
#define HOST_UV_KILL_GRACE_MS 5000

struct clm_proc {
	uv_process_t proc;
	uv_pipe_t in, out, err;
	uv_timer_t grace;
	uv_write_t wreq;
	char *stdin_buf;
	clm_proc_data_cb data;
	clm_proc_exit_cb exit;
	void *user;
	int64_t status;
	int signal;
	int handles;    /* open uv handles; the struct is freed at 0 */
	int open_pipes; /* stdout and stderr not yet at end of file */
	bool exited;
	bool detached;
	bool settled;
};

static void
host_uv_proc_close_cb(uv_handle_t *h)
{
	struct clm_proc *p = h->data;

	if (--p->handles == 0) {
		free(p->stdin_buf);
		free(p);
	}
}

static void
host_uv_proc_close(uv_handle_t *h)
{
	if (!uv_is_closing(h))
		uv_close(h, host_uv_proc_close_cb);
}

/* Run the exit callback once the child is gone and its output is drained,
 * then release every handle. */
static void
host_uv_proc_settle(struct clm_proc *p)
{
	if (p->settled || !p->exited || p->open_pipes > 0)
		return;
	p->settled = true;
	if (!p->detached && p->exit != NULL)
		p->exit(p->status, p->signal, p->user);
	uv_timer_stop(&p->grace);
	host_uv_proc_close((uv_handle_t *)&p->proc);
	host_uv_proc_close((uv_handle_t *)&p->in);
	host_uv_proc_close((uv_handle_t *)&p->out);
	host_uv_proc_close((uv_handle_t *)&p->err);
	host_uv_proc_close((uv_handle_t *)&p->grace);
}

static void
host_uv_proc_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
	(void)h;
	buf->base = malloc(suggested);
	buf->len = buf->base != NULL ? suggested : 0;
}

static void
host_uv_proc_read(uv_stream_t *s, ssize_t n, const uv_buf_t *buf)
{
	struct clm_proc *p = s->data;
	int fd = s == (uv_stream_t *)&p->out ? 1 : 2;

	if (n > 0 && !p->detached && p->data != NULL)
		p->data(fd, buf->base, (size_t)n, p->user);
	else if (n < 0) {
		uv_read_stop(s);
		p->open_pipes--;
		host_uv_proc_settle(p);
	}
	free(buf->base);
}

static void
host_uv_proc_on_exit(uv_process_t *proc, int64_t status, int sig)
{
	struct clm_proc *p = proc->data;

	p->exited = true;
	p->status = sig != 0 ? -1 : status;
	p->signal = sig;
	host_uv_proc_settle(p);
}

static void
host_uv_proc_wrote(uv_write_t *w, int status)
{
	struct clm_proc *p = w->data;

	(void)status;
	host_uv_proc_close((uv_handle_t *)&p->in);
}

static int
host_uv_proc_spawn(void *ctx, const struct clm_proc_req *req,
    clm_proc_data_cb data, clm_proc_exit_cb exit, void *user,
    struct clm_proc **out)
{
	struct host_uv_ctx *hctx = ctx;
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	struct clm_proc *p;
	int r;

	if (out != NULL)
		*out = NULL;
	if (req == NULL || req->argv == NULL || req->argv[0] == NULL)
		return -EINVAL;
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return -ENOMEM;
	if (req->stdin_data != NULL && req->stdin_len > 0) {
		p->stdin_buf = malloc(req->stdin_len);
		if (p->stdin_buf == NULL) {
			free(p);
			return -ENOMEM;
		}
		memcpy(p->stdin_buf, req->stdin_data, req->stdin_len);
	}
	p->data = data;
	p->exit = exit;
	p->user = user;

	p->proc.data = p;
	uv_pipe_init(hctx->loop, &p->in, 0);
	p->in.data = p;
	uv_pipe_init(hctx->loop, &p->out, 0);
	p->out.data = p;
	uv_pipe_init(hctx->loop, &p->err, 0);
	p->err.data = p;
	uv_timer_init(hctx->loop, &p->grace);
	p->grace.data = p;
	p->handles = 5;

	memset(&opt, 0, sizeof(opt));
	opt.file = req->argv[0];
	opt.args = (char **)req->argv;
	opt.exit_cb = host_uv_proc_on_exit;
	/* Own session and process group, so proc_kill reaches anything the
	 * child starts in the background too. */
	opt.flags = UV_PROCESS_DETACHED;
	if (p->stdin_buf != NULL) {
		stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
		stdio[0].data.stream = (uv_stream_t *)&p->in;
	} else {
		stdio[0].flags = UV_IGNORE;
	}
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&p->out;
	stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[2].data.stream = (uv_stream_t *)&p->err;
	opt.stdio = stdio;
	opt.stdio_count = 3;

	r = uv_spawn(hctx->loop, &p->proc, &opt);
	if (r < 0) {
		/* libuv still wants the failed process handle closed. */
		p->settled = true;
		host_uv_proc_close((uv_handle_t *)&p->proc);
		host_uv_proc_close((uv_handle_t *)&p->in);
		host_uv_proc_close((uv_handle_t *)&p->out);
		host_uv_proc_close((uv_handle_t *)&p->err);
		host_uv_proc_close((uv_handle_t *)&p->grace);
		return r;
	}

	p->open_pipes = 2;
	uv_read_start(
	    (uv_stream_t *)&p->out, host_uv_proc_alloc, host_uv_proc_read);
	uv_read_start(
	    (uv_stream_t *)&p->err, host_uv_proc_alloc, host_uv_proc_read);

	if (p->stdin_buf != NULL) {
		uv_buf_t b =
		    uv_buf_init(p->stdin_buf, (unsigned)req->stdin_len);

		p->wreq.data = p;
		if (uv_write(&p->wreq, (uv_stream_t *)&p->in, &b, 1,
		        host_uv_proc_wrote) < 0)
			host_uv_proc_close((uv_handle_t *)&p->in);
	} else {
		host_uv_proc_close((uv_handle_t *)&p->in);
	}

	if (out != NULL)
		*out = p;
	return 0;
}

static void
host_uv_proc_kill(struct clm_proc *p, int sig)
{
	if (p == NULL || p->exited)
		return;
	(void)uv_kill(-uv_process_get_pid(&p->proc), sig);
}

static void
host_uv_proc_grace(uv_timer_t *t)
{
	host_uv_proc_kill(t->data, SIGKILL);
}

static void
host_uv_proc_detach(struct clm_proc *p)
{
	if (p == NULL || p->detached)
		return;
	p->detached = true;
	/* Stop reading now: a background grandchild may hold the pipes open
	 * long after the child itself is gone. */
	host_uv_proc_close((uv_handle_t *)&p->out);
	host_uv_proc_close((uv_handle_t *)&p->err);
	p->open_pipes = 0;
	if (!p->exited) {
		host_uv_proc_kill(p, SIGTERM);
		uv_timer_start(
		    &p->grace, host_uv_proc_grace, HOST_UV_KILL_GRACE_MS, 0);
	}
	host_uv_proc_settle(p);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

int
clm_host_uv_new(uv_loop_t *loop, struct clm_host **out)
{
	struct clm_host *h;
	struct host_uv_ctx *hctx;

	if (loop == NULL || out == NULL)
		return -EINVAL;

	/*
	 * A write to a subprocess pipe whose reader just died (tool_shell's
	 * stdin blob, or the MCP stdio client) can hit the write() syscall at
	 * the exact moment the pipe breaks, raising SIGPIPE synchronously; the
	 * default disposition kills the whole process. libuv does not ignore
	 * this for you. We report the failure through the normal write-callback
	 * error path instead, so ignore it here, once, for any process using
	 * this desktop host.
	 */
	signal(SIGPIPE, SIG_IGN);

	hctx = calloc(1, sizeof(*hctx));
	if (hctx == NULL)
		return -ENOMEM;

	hctx->loop = loop;
	hctx->mux = clm_http_mux_new(loop);
	if (hctx->mux == NULL) {
		free(hctx);
		return -ENOMEM;
	}

	h = calloc(1, sizeof(*h));
	if (h == NULL) {
		clm_http_mux_free(hctx->mux);
		free(hctx);
		return -ENOMEM;
	}

	h->http_post = host_uv_http_post;
	h->http_cancel = host_uv_http_cancel;
	h->timer_set = host_uv_timer_set;
	h->timer_cancel = host_uv_timer_cancel;
	h->proc_spawn = host_uv_proc_spawn;
	h->proc_kill = host_uv_proc_kill;
	h->proc_detach = host_uv_proc_detach;
	h->ctx = hctx;
	/* clm_tool_invocation_loop() consumers (tool_shell/tool_bg's uv_spawn)
	 * cast this back to uv_loop_t* -- it must stay the loop itself, not
	 * hctx, whose layout is private to this adapter. */
	h->native_loop = loop;

	*out = h;
	return 0;
}

void
clm_host_uv_free(struct clm_host *host)
{
	struct host_uv_ctx *hctx;

	if (host == NULL)
		return;

	/*
	 * The core deliberately leaves one-shot probes (health, /props, and
	 * live-model lookups) unowned. They can still be attached when Ctrl-D
	 * tears the UI down, though, so discard every host-owned request before
	 * freeing the shared mux. Do not call their callbacks: their agent/UI
	 * user pointers have already been released by the caller.
	 */
	hctx = host->ctx;
	if (hctx != NULL) {
		struct host_uv_call *call;

		for (call = hctx->calls; call != NULL;) {
			struct host_uv_call *next = call->next;

			/* At this point core owners have already been
			 * destroyed, so suppress callbacks: their user pointers
			 * may be gone too. */
			if (call->req != NULL)
				clm_http_request_free(call->req);
			free(call);
			call = next;
		}
		hctx->calls = NULL;
		/* Direct request teardown removes every easy handle
		 * synchronously. */
		assert(hctx->calls == NULL);
		clm_http_mux_free(hctx->mux);
		free(hctx);
	}
	free(host);
}
