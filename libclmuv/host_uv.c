// SPDX-License-Identifier: ISC
/*
 * Desktop clm_host adapter over libcurl + libuv. Implements the transport by
 * delegating to the existing async HTTP engine (http_async.c), and timers via
 * uv_timer. See clm/host_uv.h.
 */
#include <errno.h>
#include <signal.h>
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
};

/* ------------------------------------------------------------------ */
/* HTTP transport                                                      */
/* ------------------------------------------------------------------ */

static int
host_uv_http_post(void *ctx, const struct clm_http_req *req,
                  clm_http_success_cb success, clm_http_error_cb error,
                  clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	struct host_uv_ctx *hctx = ctx;
	struct curl_slist *hdrs = NULL;
	struct clm_http_request *r = NULL;
	int rc;

	if (out != NULL)
		*out = NULL;

	/* Translate the portable "Name: Value" header list into a curl_slist.
	 * clm_http_async_post takes ownership on success. */
	if (req->headers != NULL) {
		for (const char *const *h = req->headers; *h != NULL; h++) {
			struct curl_slist *n = curl_slist_append(hdrs, *h);
			if (n == NULL) {
				curl_slist_free_all(hdrs);
				return -ENOMEM;
			}
			hdrs = n;
		}
	}

	rc = clm_http_async_post(hctx->mux, req->url, req->api_key, req->body,
	                         hdrs, success, error, data, req->client_suffix,
	                         user, &r);
	if (rc < 0) {
		/* The engine did not take the headers on a start failure. */
		curl_slist_free_all(hdrs);
		return rc;
	}
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
host_uv_timer_set(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
                  struct clm_timer **out)
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
	 * clm_http_mux_free asserts nothing is still attached (see its
	 * comment in http_async.c) -- the caller freeing this host is
	 * responsible for having already settled every request it started
	 * (cancelled or completed), same existing invariant clm_agent_free
	 * and clm_mcp_client_free already rely on for their own in-flight
	 * requests today.
	 */
	hctx = host->ctx;
	if (hctx != NULL) {
		clm_http_mux_free(hctx->mux);
		free(hctx);
	}
	free(host);
}
