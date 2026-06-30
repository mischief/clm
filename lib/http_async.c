// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <uv.h>

#include "clm/http_async.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

static size_t
http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct http_buf *buf = userp;
	char *ptr;

	ptr = realloc(buf->data, buf->len + realsize + 1);
	if (ptr == NULL)
		return 0;

	buf->data = ptr;
	memcpy(buf->data + buf->len, contents, realsize);
	buf->len += realsize;
	buf->data[buf->len] = '\0';

	return realsize;
}

static void
http_poll_callback(uv_poll_t *handle, int status, int events)
{
	struct clm_http_request *req = handle->data;
	int curl_action = 0;

	if (status < 0) {
		req->state = CLM_HTTP_ERROR;
		req->error_code = -status;
		snprintf(req->error_msg, sizeof(req->error_msg), "poll error: %s", uv_err_name(-status));
		goto done;
	}

	if (events & UV_READABLE)
		curl_action |= CURL_POLL_IN;
	if (events & UV_WRITABLE)
		curl_action |= CURL_POLL_OUT;

	curl_multi_socket_action(req->multi_handle, req->sockfd, curl_action, &req->events_pending);

	/* Check for completed transfers */
	int msgs_left;
	CURLMsg *msg;
	while ((msg = curl_multi_info_read(req->multi_handle, &msgs_left))) {
		if (msg->msg == CURLMSG_DONE) {
			CURLcode curl_err = msg->data.result;
			if (curl_err == CURLE_OK) {
				req->state = CLM_HTTP_DONE;
			} else {
				req->state = CLM_HTTP_ERROR;
				req->error_code = curl_err;
				snprintf(req->error_msg, sizeof(req->error_msg), "curl error: %s", curl_easy_strerror(curl_err));
			}
			goto done;
		}
	}

	return;

done:
	/* Stop poll */
	uv_poll_stop(handle);

	/* Call completion callback */
	if (req->state == CLM_HTTP_DONE) {
		struct clm_http_response resp = {0};
		resp.status_code = 200;
		resp.body = req->response_buf.data;
		req->response_buf.data = NULL;
		req->success_cb(&resp, req->user);
	} else {
		req->error_cb(req->error_code, req->error_msg, req->user);
	}

	clm_http_request_free(req);
}

static void
http_socket_callback(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp)
{
	struct clm_http_request *req = userp;

	if (action == CURL_POLL_IN || action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
		if (req->sockfd == CURL_SOCKET_BAD) {
			req->sockfd = s;
			uv_poll_init_socket(req->uv, &req->poll_socket, s);
			req->poll_socket.data = req;
		}

		int events = 0;
		if (action & CURL_POLL_IN)
			events |= UV_READABLE;
		if (action & CURL_POLL_OUT)
			events |= UV_WRITABLE;

		uv_poll_start(&req->poll_socket, events, http_poll_callback);
	} else if (action == CURL_POLL_REMOVE) {
		if (req->sockfd != CURL_SOCKET_BAD) {
			uv_poll_stop(&req->poll_socket);
			req->sockfd = CURL_SOCKET_BAD;
		}
	}
}

int
clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
    const char *json_body, clm_http_success_cb success_cb, clm_http_error_cb error_cb, void *user)
{
	struct clm_http_request *req;
	char auth_header[512];

	ASSERT_RETURN(loop != NULL, -EINVAL);
	ASSERT_RETURN(url != NULL, -EINVAL);
	ASSERT_RETURN(api_key != NULL, -EINVAL);
	ASSERT_RETURN(json_body != NULL, -EINVAL);
	ASSERT_RETURN(success_cb != NULL, -EINVAL);
	ASSERT_RETURN(error_cb != NULL, -EINVAL);

	req = calloc(1, sizeof(*req));
	if (req == NULL)
		return -ENOMEM;

	req->uv = loop;
	req->success_cb = success_cb;
	req->error_cb = error_cb;
	req->user = user;
	req->state = CLM_HTTP_PENDING;
	req->sockfd = CURL_SOCKET_BAD;
	req->response_buf.data = NULL;
	req->response_buf.len = 0;

	req->multi_handle = curl_multi_init();
	if (req->multi_handle == NULL) {
		free(req);
		return -ENOMEM;
	}

	req->easy_handle = curl_easy_init();
	if (req->easy_handle == NULL) {
		curl_multi_cleanup(req->multi_handle);
		free(req);
		return -ENOMEM;
	}

	if (strlen(api_key) + sizeof("Authorization: Bearer ") >= sizeof(auth_header)) {
		curl_easy_cleanup(req->easy_handle);
		curl_multi_cleanup(req->multi_handle);
		free(req);
		return -EINVAL;
	}
	(void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	req->headers = curl_slist_append(NULL, "Content-Type: application/json");
	req->headers = curl_slist_append(req->headers, auth_header);

	curl_easy_setopt(req->easy_handle, CURLOPT_URL, url);
	curl_easy_setopt(req->easy_handle, CURLOPT_POST, 1L);
	curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, http_write_callback);
	curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, &req->response_buf);
	curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->headers);
	curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, req);

	curl_multi_setopt(req->multi_handle, CURLMOPT_SOCKETFUNCTION, http_socket_callback);
	curl_multi_setopt(req->multi_handle, CURLMOPT_SOCKETDATA, req);

	curl_multi_add_handle(req->multi_handle, req->easy_handle);
	req->state = CLM_HTTP_RUNNING;

	/* Trigger initial socket action */
	curl_multi_socket_action(req->multi_handle, CURL_SOCKET_TIMEOUT, 0, &req->events_pending);

	return 0;
}

void
clm_http_request_free(struct clm_http_request *req)
{
	if (req == NULL)
		return;

	if (req->easy_handle != NULL) {
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
