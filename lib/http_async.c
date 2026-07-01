// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <uv.h>

#include "clm/http_async.h"
#include "clm/log.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "version.h"
#include "banned.h"

/* Base User-Agent: product token, version, and a homepage comment. Kept
 * minimal (no OS/arch/stack leakage) by design; an optional per-request
 * "(tool: <name>)" comment is appended for tool/plugin attribution. */
#define CLM_HOMEPAGE "https://github.com/mischief/clm"
#define CLM_UA_BASE "clm/" CLM_VERSION " (+" CLM_HOMEPAGE ")"

static size_t
http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct clm_http_request *req = userp;
	struct http_buf *buf = &req->response_buf;
	char *ptr;

	ptr = realloc(buf->data, buf->len + realsize + 1);
	if (ptr == NULL)
		return 0;

	buf->data = ptr;
	memcpy(buf->data + buf->len, contents, realsize);
	buf->len += realsize;
	buf->data[buf->len] = '\0';

	/* Forward chunks to a streaming consumer, but only for a 2xx response so
	 * it never sees an error body. */
	if (req->data_cb != NULL) {
		long code = 0;
		curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &code);
		if (code >= 200 && code < 300)
			req->data_cb(contents, realsize, req->user);
	}

	return realsize;
}

static void http_handle_closed(uv_handle_t *handle);
static void http_request_teardown(struct clm_http_request *req);
static int http_timer_callback(CURLM *multi, long timeout_ms, void *userp);
static void http_timer_expired(uv_timer_t *handle);

/*
 * Drain curl's message queue after a socket_action. If a transfer finished,
 * record its outcome on req and return true so the caller tears down. Both the
 * socket-poll and timer paths must call this: a transfer can complete off the
 * timer (e.g. TLS/handshake progress or a fast response driven by a 0ms
 * timeout), and if only the poll path reaps CURLMSG_DONE the timer keeps
 * re-arming at 0ms and the loop spins at 100% CPU.
 */
static bool
http_reap_done(struct clm_http_request *req)
{
	int msgs_left;
	CURLMsg *msg;

	while ((msg = curl_multi_info_read(req->multi_handle, &msgs_left)) != NULL) {
		if (msg->msg != CURLMSG_DONE)
			continue;
		CURLcode curl_err = msg->data.result;
		if (curl_err == CURLE_OK) {
			req->state = CLM_HTTP_DONE;
			clm_debug("CURLMSG_DONE, CURLE_OK");
		} else {
			req->state = CLM_HTTP_ERROR;
			req->error_code = (int)curl_err;
			snprintf(req->error_msg, sizeof(req->error_msg),
			    "curl error: %s", curl_easy_strerror(curl_err));
			clm_debug("CURLMSG_DONE, curl_err=%d", curl_err);
		}
		return true;
	}
	return false;
}

static void
http_poll_callback(uv_poll_t *handle, int status, int events)
{
	struct clm_http_socket *ctx = handle->data;
	struct clm_http_request *req = ctx->req;
	int curl_action = 0;

	clm_debug("status=%d, events=%d, fd=%d", status, events, ctx->sockfd);

	if (status < 0) {
		req->state = CLM_HTTP_ERROR;
		req->error_code = -status;
		snprintf(req->error_msg, sizeof(req->error_msg), "poll error: %s", uv_err_name(-status));
		clm_debug("poll error, status=%d", status);
		goto done;
	}

	if (events & UV_READABLE)
		curl_action |= CURL_POLL_IN;
	if (events & UV_WRITABLE)
		curl_action |= CURL_POLL_OUT;

	clm_debug("curl_action=%d", curl_action);

	curl_multi_socket_action(req->multi_handle, ctx->sockfd, curl_action, &req->events_pending);
	clm_debug("curl_multi_socket_action completed");

	if (http_reap_done(req))
		goto done;

	return;

done:
	clm_debug("calling http_request_teardown");
	http_request_teardown(req);
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
	struct clm_http_request *req = userp;
	struct clm_http_socket *ctx = socketp;

	clm_debug("action=%d, s=%d", action, s);

	if (action == CURL_POLL_REMOVE) {
		if (ctx != NULL) {
			uv_poll_stop(&ctx->poll);
			uv_close((uv_handle_t *)&ctx->poll, on_socket_closed);
			curl_multi_assign(req->multi_handle, s, NULL);
		}
		return 0;
	}

	if (ctx == NULL) {
		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL)
			return 0;
		ctx->sockfd = s;
		ctx->req = req;
		ctx->poll.data = ctx;
		uv_poll_init_socket(req->uv, &ctx->poll, s);
		curl_multi_assign(req->multi_handle, s, ctx);
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

int
clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
    const char *json_body, struct curl_slist *extra_headers,
    clm_http_success_cb success_cb, clm_http_error_cb error_cb,
    clm_http_data_cb data_cb, const char *client_suffix, void *user,
    struct clm_http_request **out_req)
{
	struct clm_http_request *req;
	char auth_header[512];

	clm_debug("starting");

	if (out_req != NULL)
		*out_req = NULL;

	ASSERT_RETURN(loop != NULL, -EINVAL);
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

	req->uv = loop;
	req->success_cb = success_cb;
	req->error_cb = error_cb;
	req->data_cb = data_cb;
	req->user = user;
	req->state = CLM_HTTP_PENDING;
	req->response_buf.data = NULL;
	req->response_buf.len = 0;

	req->multi_handle = curl_multi_init();
	if (req->multi_handle == NULL) {
		clm_debug("curl_multi_init failed");
		free(req);
		return -ENOMEM;
	}
	clm_debug("curl_multi_init success");

	req->easy_handle = curl_easy_init();
	if (req->easy_handle == NULL) {
		clm_debug("curl_easy_init failed");
		curl_multi_cleanup(req->multi_handle);
		free(req);
		return -ENOMEM;
	}
	clm_debug("curl_easy_init success");

	if (api_key != NULL && api_key[0] != '\0') {
		if (strlen(api_key) + sizeof("Authorization: Bearer ") >= sizeof(auth_header)) {
			clm_debug("auth_header too long");
			curl_easy_cleanup(req->easy_handle);
			curl_multi_cleanup(req->multi_handle);
			free(req);
			return -EINVAL;
		}
		(void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
		req->headers = curl_slist_append(NULL, "Content-Type: application/json");
		req->headers = curl_slist_append(req->headers, auth_header);
	} else {
		req->headers = NULL;
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
		curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDS, json_body);
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
	curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(req->easy_handle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, req);

	curl_multi_setopt(req->multi_handle, CURLMOPT_SOCKETFUNCTION, http_socket_callback);
	curl_multi_setopt(req->multi_handle, CURLMOPT_SOCKETDATA, req);
	curl_multi_setopt(req->multi_handle, CURLMOPT_TIMERFUNCTION, http_timer_callback);
	curl_multi_setopt(req->multi_handle, CURLMOPT_TIMERDATA, req);

	curl_multi_add_handle(req->multi_handle, req->easy_handle);
	req->state = CLM_HTTP_RUNNING;
	clm_debug("curl_multi_add_handle success");

	/* Hand the caller a cancellable handle before we start driving it. The
	 * transfer only completes on a later loop iteration, so this is safe. */
	if (out_req != NULL)
		*out_req = req;

	/* Trigger initial socket action */
	curl_multi_socket_action(req->multi_handle, CURL_SOCKET_TIMEOUT, 0, &req->events_pending);
	clm_debug("curl_multi_socket_action completed");

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

static void
http_handle_closed(uv_handle_t *handle)
{
	struct clm_http_request *req = handle->data;
	req->handles_to_close--;
	if (req->handles_to_close == 0) {
		http_request_complete(req);
		clm_http_request_free(req);
	}
}

static void
http_request_teardown(struct clm_http_request *req)
{
	clm_debug("closing=%d", req->closing);

	if (req->closing)
		return;
	req->closing = 1;

	/* Remove the easy handle from multi; this triggers CURL_POLL_REMOVE
	 * callbacks for any open sockets, which close their poll handles. */
	if (req->easy_handle != NULL && req->multi_handle != NULL)
		curl_multi_remove_handle(req->multi_handle, req->easy_handle);

	req->handles_to_close = 0;
	if (req->timer_initialized)
		req->handles_to_close++;

	if (req->handles_to_close == 0) {
		clm_debug("no handles to close, completing");
		http_request_complete(req);
		clm_http_request_free(req);
		return;
	}
	if (req->timer_initialized) {
		clm_debug("uv_close timer_handle");
		uv_close((uv_handle_t *)&req->timer_handle, http_handle_closed);
	}
}

static int
http_timer_callback(CURLM *multi, long timeout_ms, void *userp)
{
	struct clm_http_request *req = userp;

	clm_debug("timeout_ms=%ld", timeout_ms);

	if (timeout_ms < 0) {
		if (req->timer_initialized) {
			uv_timer_stop(&req->timer_handle);
			clm_debug("uv_timer_stop completed");
		}
		return 0;
	}

	if (!req->timer_initialized) {
		uv_timer_init(req->uv, &req->timer_handle);
		req->timer_handle.data = req;
		req->timer_initialized = 1;
		clm_debug("uv_timer_init success");
	}

	uv_timer_start(&req->timer_handle, http_timer_expired, (uint64_t)timeout_ms, 0);
	clm_debug("uv_timer_start completed with timeout=%lu", (unsigned long)timeout_ms);
	return 0;
}

static void
http_timer_expired(uv_timer_t *handle)
{
	struct clm_http_request *req = handle->data;
	int running;
	clm_debug("calling curl_multi_socket_action");
	curl_multi_socket_action(req->multi_handle, CURL_SOCKET_TIMEOUT, 0, &running);
	clm_debug("curl_multi_socket_action completed, running=%d", running);

	/*
	 * A transfer can finish on the timer path. Reap it here too, else a
	 * curl-requested 0ms timeout re-arms forever and the loop spins.
	 */
	if (http_reap_done(req))
		http_request_teardown(req);
}

void
clm_http_request_free(struct clm_http_request *req)
{
	if (req == NULL)
		return;

	if (req->easy_handle != NULL) {
		/* Remove from multi if not already done in teardown. */
		if (req->multi_handle != NULL)
			curl_multi_remove_handle(req->multi_handle, req->easy_handle);
		curl_easy_cleanup(req->easy_handle);
		req->easy_handle = NULL;
	}

	if (req->multi_handle != NULL) {
		curl_multi_cleanup(req->multi_handle);
		req->multi_handle = NULL;
	}

	if (req->headers != NULL) {
		curl_slist_free_all(req->headers);
		req->headers = NULL;
	}

	if (req->response_buf.data != NULL) {
		free(req->response_buf.data);
		req->response_buf.data = NULL;
	}

	free(req);
}

void
clm_http_response_free(struct clm_http_response *resp)
{
	if (resp == NULL)
		return;
	free(resp->body);
	free(resp->error_msg);
	resp->body = NULL;
	resp->error_msg = NULL;
	resp->status_code = 0;
}
