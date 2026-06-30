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

/* Async HTTP request response buffer */
struct http_buf {
	char *data;
	size_t len;
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
	
	/* UV socket poll handle */
	uv_poll_t poll_socket;
	curl_socket_t sockfd;
	int events_pending;
	
	/* Callbacks and user data */
	clm_http_success_cb success_cb;
	clm_http_error_cb error_cb;
	void *user;
	
	/* State */
	enum clm_http_request_state state;
	int error_code;
	char error_msg[256];
};

/*
 * Initiate an async HTTP POST request.
 * 
 * Returns 0 on success (request started), negative errno on failure.
 * On completion, either success_cb or error_cb will be called.
 */
int clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
    const char *json_body, clm_http_success_cb success_cb, clm_http_error_cb error_cb, void *user);

/*
 * Free an async HTTP request and cleanup resources.
 * Called internally by the completion callbacks.
 */
void clm_http_request_free(struct clm_http_request *req);

#endif /* CLM_HTTP_ASYNC_H */
