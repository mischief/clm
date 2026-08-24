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
