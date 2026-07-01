// SPDX-License-Identifier: ISC
#ifndef CLM_HTTP_ASYNC_H
#define CLM_HTTP_ASYNC_H

#include <curl/curl.h>
#include <uv.h>

#include "clm/http.h"

struct clm_http_request;

/* HTTP request state */
enum clm_http_request_state {
	CLM_HTTP_PENDING,
	CLM_HTTP_RUNNING,
	CLM_HTTP_DONE,
	CLM_HTTP_ERROR,
};

/* Callback when HTTP request completes successfully */
typedef void (*clm_http_success_cb)(struct clm_http_response *resp, void *user);

/* Callback when HTTP request fails */
typedef void (*clm_http_error_cb)(int error_code, const char *error_msg, void *user);

/*
 * Optional callback delivering response body bytes as they arrive. Only
 * invoked for a 2xx response, so a streaming consumer never sees error bodies.
 * The full body is still accumulated and handed to success_cb at the end.
 */
typedef void (*clm_http_data_cb)(const char *data, size_t len, void *user);

/* Async HTTP request response buffer */
struct http_buf {
	char *data;
	size_t len;
};

/* Per-socket poll context, allocated by the socket callback. */
struct clm_http_socket {
	uv_poll_t poll;
	curl_socket_t sockfd;
	struct clm_http_request *req;
};

/* Async HTTP request context */
struct clm_http_request {
	uv_loop_t *uv;
	
	/* Curl handles */
	CURLM *multi_handle;
	CURL *easy_handle;
	struct curl_slist *headers;
	
	/* Response buffer */
	struct http_buf response_buf;
	
	int events_pending;
	
	/* UV timer handle */
	uv_timer_t timer_handle;
	int timer_initialized;
	
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
	int handles_to_close;
};

/*
 * Initiate an async HTTP POST request.
 *
 * client_suffix, when non-NULL, is appended to the User-Agent as a comment
 * "(tool: <suffix>)" so the server can attribute the request to a specific
 * tool/plugin. Pass NULL for the base User-Agent only.
 *
 * Returns 0 on success (request started), negative errno on failure.
 * On completion, either success_cb or error_cb will be called.
 */
int clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
    const char *json_body, clm_http_success_cb success_cb, clm_http_error_cb error_cb,
    clm_http_data_cb data_cb, const char *client_suffix, void *user,
    struct clm_http_request **out_req);

/*
 * Abort an in-flight request. Tears down its handles and delivers the outcome
 * to error_cb with -ECANCELED. Safe to call once, before the request has
 * completed; a no-op if already tearing down.
 */
void clm_http_async_cancel(struct clm_http_request *req);

/*
 * Free an async HTTP request and cleanup resources.
 * Called internally by the completion callbacks.
 */
void clm_http_request_free(struct clm_http_request *req);

#endif /* CLM_HTTP_ASYNC_H */
