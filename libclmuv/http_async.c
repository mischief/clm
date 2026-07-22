// SPDX-License-Identifier: ISC
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <uv.h>

#include "clm/http_async.h"
#include "clm/log.h"
#include "clm/cleanup.h"
#include "http_response_buffer.h"
#include "useful.h"
#include "version.h"
#include "banned.h"

_Static_assert(CLM_HTTP_MAX_RESPONSE_BYTES > 0,
    "CLM_HTTP_MAX_RESPONSE_BYTES must be positive");
_Static_assert((uintmax_t)CLM_HTTP_MAX_RESPONSE_BYTES <= (uintmax_t)SIZE_MAX,
    "CLM_HTTP_MAX_RESPONSE_BYTES must fit in size_t");

/* Base User-Agent: product token, version, and a homepage comment. Kept
 * minimal (no OS/arch/stack leakage) by design; an optional per-request
 * "(tool: <name>)" comment is appended for tool/plugin attribution. */
#define CLM_HOMEPAGE "https://github.com/mischief/clm"
#define CLM_UA_BASE "clm/" CLM_VERSION " (+" CLM_HOMEPAGE ")"

/* HTTP request state. */
enum clm_http_request_state {
	CLM_HTTP_PENDING,
	CLM_HTTP_RUNNING,
	CLM_HTTP_DONE,
	CLM_HTTP_ERROR,
};

/* per-socket poll context, allocated by the socket callback. one per fd
 * curl asks us to watch; outlives any single request, since curl reuses
 * a kept-alive connection's fd across requests on the shared multi. */
struct clm_http_socket {
	uv_poll_t poll;
	curl_socket_t sockfd;
	struct clm_http_mux *mux;
};

/*
 * Shared transport context: one CURLM multi handle plus the one uv_timer_t
 * curl_multi ever needs (CURLMOPT_TIMERFUNCTION gives a single next-wakeup
 * per multi handle, not per easy handle, so the timer lives here, not on
 * individual requests). See clm/http_async.h for the sharing/ownership
 * contract.
 *
 * live_requests is not a refcount in the "free when it hits zero" sense --
 * the mux is only ever freed by its owner's explicit clm_http_mux_free()
 * call, never by requests finishing. It exists purely so that call can
 * assert nothing is still attached, catching a too-early free (a real bug:
 * it would leave any still-running request holding a dangling multi_handle)
 * immediately instead of as a later use-after-free.
 */
struct clm_http_mux {
	uv_loop_t *uv;
	CURLM *multi_handle;
	uv_timer_t timer_handle;
	bool timer_initialized;
	bool timer_closing; /* uv_close on timer_handle issued, awaiting callback */
	size_t live_requests;
};

/* Async HTTP request context (opaque to callers; see clm/http_async.h). */
struct clm_http_request {
	struct clm_http_mux *mux; /* borrowed; outlives the request */

	/* Curl handle */
	CURL *easy_handle;
	struct curl_slist *headers;

	/* Response buffer */
	struct http_response_buffer response_buf;

	int events_pending;

	/* Callbacks and user data */
	clm_http_success_cb success_cb;
	clm_http_error_cb error_cb;
	clm_http_data_cb data_cb;
	void *user;

	/* State */
	enum clm_http_request_state state;
	int error_code;
	char error_msg[256];
	int closing;
	bool starting;
};

static size_t
http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	struct clm_http_request *req = userp;
	enum http_response_buffer_result result;
	size_t realsize;
	long code = 0;
	bool streaming;

	curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &code);
	streaming = req->data_cb != NULL && code >= 200 && code < 300;
	if (streaming && req->response_buf.data != NULL) {
		free(req->response_buf.data);
		req->response_buf.data = NULL;
		req->response_buf.len = 0;
	}
	result = http_response_buffer_write(&req->response_buf, contents, size,
	    nmemb, !streaming, &realsize);
	if (result != HTTP_RESPONSE_BUFFER_OK) {
		req->state = CLM_HTTP_ERROR;
		switch (result) {
		case HTTP_RESPONSE_BUFFER_OVERFLOW:
			req->error_code = -EOVERFLOW;
			(void)snprintf(req->error_msg, sizeof(req->error_msg),
			    "http response size overflow");
			break;
		case HTTP_RESPONSE_BUFFER_TOO_LARGE:
			req->error_code = -EFBIG;
			(void)snprintf(req->error_msg, sizeof(req->error_msg),
			    "http response exceeds %zu-byte limit",
			    req->response_buf.limit);
			break;
		case HTTP_RESPONSE_BUFFER_NO_MEMORY:
			req->error_code = -ENOMEM;
			(void)snprintf(req->error_msg, sizeof(req->error_msg),
			    "out of memory buffering http response");
			break;
		case HTTP_RESPONSE_BUFFER_OK:
		default:
			req->error_code = -EIO;
			(void)snprintf(req->error_msg, sizeof(req->error_msg),
			    "http response write failed");
			break;
		}
		return 0;
	}

	if (streaming)
		req->data_cb(contents, realsize, req->user);

	return realsize;
}

static void http_request_complete(struct clm_http_request *req);
static void http_request_teardown(struct clm_http_request *req);
static int http_timer_callback(CURLM *multi, long timeout_ms, void *userp);
static void http_timer_expired(uv_timer_t *handle);

/*
 * Look up the clm_http_request an in-progress easy handle belongs to. Every
 * easy handle gets CURLOPT_PRIVATE set to its owning req at creation, so this
 * is how the shared mux's callbacks (which only ever get the mux itself as
 * their userp/timerdata -- see CURLMOPT_SOCKETDATA/TIMERDATA below) recover
 * which *request*, out of however many are concurrently multiplexed on this
 * one CURLM, a given socket event or completion message is actually for.
 */
static struct clm_http_request *
req_from_easy(CURL *easy)
{
	struct clm_http_request *req = NULL;
	curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
	return req;
}

/*
 * Drain curl's message queue after a socket_action, tearing down every
 * transfer that finished. A single curl_multi_socket_action on a mux shared
 * by several concurrent requests can complete more than one of them at once
 * (e.g. two tool-call HTTP requests finishing in the same poll wakeup), so
 * this must fully drain the queue -- not stop at the first CURLMSG_DONE --
 * and resolve each message's own request via req_from_easy rather than
 * assuming it's "the" request the caller already had in hand.
 *
 * Both the socket-poll and timer paths call this: a transfer can complete off
 * the timer (e.g. TLS/handshake progress or a fast response driven by a 0ms
 * timeout), and if only the poll path reaped CURLMSG_DONE the timer would
 * keep re-arming at 0ms and the loop would spin at 100% CPU.
 */
static void
http_reap_done(struct clm_http_mux *mux, int poll_status)
{
	int msgs_left;
	CURLMsg *msg;

	while ((msg = curl_multi_info_read(mux->multi_handle, &msgs_left)) != NULL) {
		struct clm_http_request *req;

		if (msg->msg != CURLMSG_DONE)
			continue;

		req = req_from_easy(msg->easy_handle);
		if (req == NULL) {
			clm_debug("CURLMSG_DONE for unknown easy handle, ignoring");
			continue;
		}

		CURLcode curl_err = msg->data.result;
		if (poll_status < 0) {
			req->state = CLM_HTTP_ERROR;
			req->error_code = poll_status;
			snprintf(req->error_msg, sizeof(req->error_msg),
			    "poll error: %s", uv_err_name(poll_status));
			clm_debug("poll error, status=%d", poll_status);
		} else if (curl_err == CURLE_OK) {
			req->state = CLM_HTTP_DONE;
			clm_debug("CURLMSG_DONE, CURLE_OK");
		} else if (req->state != CLM_HTTP_ERROR) {
			req->state = CLM_HTTP_ERROR;
			req->error_code = (int)curl_err;
			snprintf(req->error_msg, sizeof(req->error_msg),
			    "curl error: %s", curl_easy_strerror(curl_err));
			clm_debug("CURLMSG_DONE, curl_err=%d", curl_err);
		}
		http_request_teardown(req);
	}
}

static void
http_poll_callback(uv_poll_t *handle, int status, int events)
{
	struct clm_http_socket *ctx = handle->data;
	struct clm_http_mux *mux = ctx->mux;
	int curl_action = 0;
	int running = 0;

	clm_debug("status=%d, events=%d, fd=%d", status, events, ctx->sockfd);

	if (status < 0) {
		/* tell curl this socket failed, then retain libuv's signed status for
		 * every request completed by this action. */
		curl_multi_socket_action(mux->multi_handle, ctx->sockfd,
		    CURL_CSELECT_ERR, &running);
		http_reap_done(mux, status);
		return;
	}

	if (events & UV_READABLE)
		curl_action |= CURL_POLL_IN;
	if (events & UV_WRITABLE)
		curl_action |= CURL_POLL_OUT;

	clm_debug("curl_action=%d", curl_action);

	curl_multi_socket_action(mux->multi_handle, ctx->sockfd, curl_action, &running);
	clm_debug("curl_multi_socket_action completed");

	http_reap_done(mux, 0);
}

static void
on_socket_closed(uv_handle_t *handle)
{
	struct clm_http_socket *ctx = handle->data;
	free(ctx);
}

static int
http_socket_callback(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp)
{
	struct clm_http_mux *mux = userp;
	struct clm_http_socket *ctx = socketp;

	(void)easy;
	clm_debug("action=%d, s=%d", action, s);

	if (action == CURL_POLL_REMOVE) {
		if (ctx != NULL) {
			uv_poll_stop(&ctx->poll);
			uv_close((uv_handle_t *)&ctx->poll, on_socket_closed);
			curl_multi_assign(mux->multi_handle, s, NULL);
		}
		return 0;
	}

	if (ctx == NULL) {
		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL)
			return 0;
		ctx->sockfd = s;
		ctx->mux = mux;
		ctx->poll.data = ctx;
		uv_poll_init_socket(mux->uv, &ctx->poll, s);
		curl_multi_assign(mux->multi_handle, s, ctx);
		clm_debug("uv_poll_init_socket success, sockfd=%d", s);
	}

	int events = 0;
	if (action & CURL_POLL_IN)
		events |= UV_READABLE;
	if (action & CURL_POLL_OUT)
		events |= UV_WRITABLE;

	clm_debug("events=%d", events);

	if (!uv_is_closing((uv_handle_t *)&ctx->poll))
		uv_poll_start(&ctx->poll, events, http_poll_callback);
	return 0;
}

static int
http_timer_callback(CURLM *multi, long timeout_ms, void *userp)
{
	struct clm_http_mux *mux = userp;

	(void)multi;
	clm_debug("timeout_ms=%ld", timeout_ms);

	if (mux->timer_closing)
		return 0; /* mux is tearing down; nothing to (re)arm */

	if (timeout_ms < 0) {
		if (mux->timer_initialized) {
			uv_timer_stop(&mux->timer_handle);
			clm_debug("uv_timer_stop completed");
		}
		return 0;
	}

	if (!mux->timer_initialized) {
		uv_timer_init(mux->uv, &mux->timer_handle);
		mux->timer_handle.data = mux;
		mux->timer_initialized = true;
		clm_debug("uv_timer_init success");
	}

	uv_timer_start(&mux->timer_handle, http_timer_expired, (uint64_t)timeout_ms, 0);
	clm_debug("uv_timer_start completed with timeout=%lu", (unsigned long)timeout_ms);
	return 0;
}

static void
http_timer_expired(uv_timer_t *handle)
{
	struct clm_http_mux *mux = handle->data;
	int running;

	clm_debug("calling curl_multi_socket_action");
	curl_multi_socket_action(mux->multi_handle, CURL_SOCKET_TIMEOUT, 0, &running);
	clm_debug("curl_multi_socket_action completed, running=%d", running);

	/* A transfer can finish on the timer path; see http_reap_done's
	 * comment for why this must be drained here too. */
	http_reap_done(mux, 0);
}

struct clm_http_mux *
clm_http_mux_new(uv_loop_t *loop)
{
	struct clm_http_mux *mux;

	if (loop == NULL)
		return NULL;

	mux = calloc(1, sizeof(*mux));
	if (mux == NULL)
		return NULL;

	mux->multi_handle = curl_multi_init();
	if (mux->multi_handle == NULL) {
		free(mux);
		return NULL;
	}

	mux->uv = loop;

	curl_multi_setopt(mux->multi_handle, CURLMOPT_SOCKETFUNCTION, http_socket_callback);
	curl_multi_setopt(mux->multi_handle, CURLMOPT_SOCKETDATA, mux);
	curl_multi_setopt(mux->multi_handle, CURLMOPT_TIMERFUNCTION, http_timer_callback);
	curl_multi_setopt(mux->multi_handle, CURLMOPT_TIMERDATA, mux);

	return mux;
}

static void
mux_free_now(uv_handle_t *handle)
{
	struct clm_http_mux *mux = handle->data;

	assert(mux->live_requests == 0);
	curl_multi_cleanup(mux->multi_handle);
	free(mux);
}

void
clm_http_mux_free(struct clm_http_mux *mux)
{
	if (mux == NULL)
		return;

	/*
	 * Every request started against this mux must already be gone --
	 * clm_http_request_free (called from every completion/cancel/
	 * teardown path) decrements live_requests, and nothing else does.
	 * A caller that frees the mux while a request is still attached has
	 * a lifetime bug: continuing would hand that request's later
	 * callbacks (socket/timer events still pointed at this mux) a
	 * dangling CURLM*. Catch it here, loudly, instead of corrupting
	 * memory silently later.
	 */
	assert(mux->live_requests == 0);

	if (mux->timer_initialized && !mux->timer_closing) {
		mux->timer_closing = true;
		uv_close((uv_handle_t *)&mux->timer_handle, mux_free_now);
		return;
	}

	/* No timer handle was ever created (never had a request), or it was
	 * already stopped/never started closing: nothing pending on the
	 * loop for this mux, free it immediately. */
	curl_multi_cleanup(mux->multi_handle);
	free(mux);
}

int
clm_http_async_post(struct clm_http_mux *mux, const char *url,
    const char *api_key, const char *json_body,
    struct curl_slist *extra_headers, clm_http_success_cb success_cb,
    clm_http_error_cb error_cb, clm_http_data_cb data_cb,
    const char *client_suffix, void *user,
    struct clm_http_request **out_req)
{
	struct clm_http_request *req;
	char *auth_header;
	size_t auth_header_len;

	clm_debug("starting");

	if (out_req != NULL)
		*out_req = NULL;

	ASSERT_RETURN(mux != NULL, -EINVAL);
	ASSERT_RETURN(url != NULL, -EINVAL);
	/* json_body == NULL performs a GET (used for the health check). */
	ASSERT_RETURN(success_cb != NULL, -EINVAL);
	ASSERT_RETURN(error_cb != NULL, -EINVAL);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		clm_debug("calloc failed");
		return -ENOMEM;
	}
	clm_debug("req allocated");

	req->mux = mux;
	req->success_cb = success_cb;
	req->error_cb = error_cb;
	req->data_cb = data_cb;
	req->user = user;
	req->state = CLM_HTTP_PENDING;
	req->response_buf.data = NULL;
	req->response_buf.len = 0;
	req->response_buf.received = 0;
	req->response_buf.limit = (size_t)CLM_HTTP_MAX_RESPONSE_BYTES;

	req->easy_handle = curl_easy_init();
	if (req->easy_handle == NULL) {
		clm_debug("curl_easy_init failed");
		free(req);
		return -ENOMEM;
	}
	clm_debug("curl_easy_init success");

	/* Content-Type is unconditional: whether or not this connection needs
	 * a bearer token, the body is always a JSON POST. This used to be set
	 * only inside the api_key branch below -- fine for any provider with
	 * a real key, but a provider explicitly configured with an empty key
	 * (e.g. secrets.lua's llm7 = "", a real "no key needed" free-tier
	 * entry, not an unconfigured one) sent no Content-Type at all, and
	 * more than one backend (llm7.io included) 400s a JSON POST with no
	 * Content-Type rather than inferring it from the body. */
	req->headers = curl_slist_append(NULL, "Content-Type: application/json");
	if (api_key != NULL && api_key[0] != '\0') {
		auth_header_len = sizeof("Authorization: Bearer ") + strlen(api_key);
		auth_header = malloc(auth_header_len);
		if (auth_header == NULL) {
			clm_debug("auth_header alloc failed");
			curl_slist_free_all(req->headers);
			curl_easy_cleanup(req->easy_handle);
			free(req);
			return -ENOMEM;
		}
		(void)snprintf(auth_header, auth_header_len, "Authorization: Bearer %s", api_key);
		req->headers = curl_slist_append(req->headers, auth_header);
		free(auth_header);
	}

	/* Append any caller-provided extra headers. */
	if (extra_headers != NULL) {
		struct curl_slist *h;
		for (h = extra_headers; h != NULL; h = h->next)
			req->headers = curl_slist_append(req->headers, h->data);
		curl_slist_free_all(extra_headers);
	}

	curl_easy_setopt(req->easy_handle, CURLOPT_URL, url);
	if (json_body != NULL) {
		curl_easy_setopt(req->easy_handle, CURLOPT_POST, 1L);
		/* COPY, not POSTFIELDS: the latter only stores the pointer and
		 * reads it later when the async transfer actually runs, but
		 * callers (eg mcp_http_send) free their body buffer right
		 * after this call returns -- a use-after-free that sent
		 * garbage bytes as the request body instead of the caller's
		 * JSON. */
		curl_easy_setopt(req->easy_handle, CURLOPT_COPYPOSTFIELDS, json_body);
	} else {
		curl_easy_setopt(req->easy_handle, CURLOPT_HTTPGET, 1L);
	}
	curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, http_write_callback);
	curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, req);
	if (req->headers != NULL)
		curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->headers);

	/* Minimal, deliberate User-Agent; append a tool/plugin comment if given. */
	if (client_suffix != NULL && client_suffix[0] != '\0') {
		char ua[256];
		(void)snprintf(ua, sizeof(ua), "%s (tool: %s)", CLM_UA_BASE,
		    client_suffix);
		curl_easy_setopt(req->easy_handle, CURLOPT_USERAGENT, ua);
	} else {
		curl_easy_setopt(req->easy_handle, CURLOPT_USERAGENT, CLM_UA_BASE);
	}
	curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, 0L);
	/* No total timeout, but abort if the connection goes completely dead.
	 * 300s (not 120s) because a large prompt against a slow/local model
	 * can legitimately sit silent for minutes during prefill before the
	 * server emits its first byte -- this bit real compaction calls
	 * (whose prompt is the whole conversation history, the largest single
	 * request in a session) and ordinary turn calls once context grew
	 * large: curl aborted mid-prefill on a perfectly healthy connection,
	 * discarding 100+ seconds of already-done prefill and forcing a cold
	 * restart on the next call. Still catches a truly dead connection,
	 * just after a longer wait. */
	curl_easy_setopt(req->easy_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(req->easy_handle, CURLOPT_LOW_SPEED_TIME, 300L);
	curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(req->easy_handle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	/* CURLOPT_PRIVATE, not CURLMOPT_SOCKETDATA/TIMERDATA (those carry the
	 * mux, set once in clm_http_mux_new): this is how the mux's shared
	 * callbacks recover *this* request out of however many are
	 * concurrently multiplexed on the same CURLM -- see req_from_easy. */
	curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, req);

	curl_multi_add_handle(mux->multi_handle, req->easy_handle);
	req->state = CLM_HTTP_RUNNING;
	mux->live_requests++;
	clm_debug("curl_multi_add_handle success");

	/* the initial action may complete inline. keep the request alive until
	 * this function can either publish a live handle or return a completed
	 * request with a null handle. */
	req->starting = true;
	curl_multi_socket_action(mux->multi_handle, CURL_SOCKET_TIMEOUT, 0,
	    &req->events_pending);
	clm_debug("curl_multi_socket_action completed");

	http_reap_done(mux, 0);

	req->starting = false;
	if (req->closing) {
		http_request_complete(req);
		clm_http_request_free(req);
		return 0;
	}

	if (out_req != NULL)
		*out_req = req;
	return 0;
}

void
clm_http_async_cancel(struct clm_http_request *req)
{
	if (req == NULL || req->closing)
		return;
	/* Report the abort through the error path, then tear down. */
	req->state = CLM_HTTP_ERROR;
	req->error_code = -ECANCELED;
	(void)snprintf(req->error_msg, sizeof(req->error_msg), "cancelled");
	http_request_teardown(req);
}

static void
http_request_complete(struct clm_http_request *req)
{
	if (req->state == CLM_HTTP_DONE) {
		struct clm_http_response resp = {0};
		long status_code = 200;
		curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &status_code);
		resp.status_code = (int)status_code;
		resp.body = req->response_buf.data;
		req->response_buf.data = NULL;
		req->success_cb(&resp, req->user);
	} else {
		req->error_cb(req->error_code, req->error_msg, req->user);
	}
}

/*
 * Remove the easy handle from the shared multi and finish the request
 * synchronously. There is no per-request timer/socket handle to wait on
 * anymore (those belong to the mux, shared across every request, and
 * outlive this one) -- curl_multi_remove_handle is synchronous, so once it
 * returns there is nothing left pending specifically for this request, and
 * clm_http_request_free can run immediately instead of deferring to a uv
 * close callback the way the old per-request-multi design had to.
 */
static void
http_request_teardown(struct clm_http_request *req)
{
	clm_debug("closing=%d", req->closing);

	if (req->closing)
		return;
	req->closing = 1;

	if (req->easy_handle != NULL && req->mux != NULL &&
	    req->mux->multi_handle != NULL)
		curl_multi_remove_handle(req->mux->multi_handle, req->easy_handle);

	/* detach before invoking user code. the callback may release the mux owner. */
	if (req->mux != NULL) {
		assert(req->mux->live_requests > 0);
		req->mux->live_requests--;
		req->mux = NULL;
	}

	if (req->starting) {
		clm_debug("completion deferred until start returns");
		return;
	}

	http_request_complete(req);
	clm_http_request_free(req);
}

void
clm_http_request_free(struct clm_http_request *req)
{
	if (req == NULL)
		return;

	if (req->easy_handle != NULL) {
		/* Remove from the mux's multi if not already done in
		 * teardown (e.g. a caller invoking this directly on a
		 * request that was never started). */
		if (req->mux != NULL && req->mux->multi_handle != NULL)
			curl_multi_remove_handle(req->mux->multi_handle, req->easy_handle);
		curl_easy_cleanup(req->easy_handle);
		req->easy_handle = NULL;
	}

	if (req->headers != NULL) {
		curl_slist_free_all(req->headers);
		req->headers = NULL;
	}

	if (req->response_buf.data != NULL) {
		free(req->response_buf.data);
		req->response_buf.data = NULL;
	}

	/* The mux itself is borrowed, never owned by a request -- only
	 * live_requests is ours to touch, so its owner's clm_http_mux_free
	 * can assert nothing is still attached. */
	if (req->mux != NULL) {
		assert(req->mux->live_requests > 0);
		req->mux->live_requests--;
	}

	free(req);
}
