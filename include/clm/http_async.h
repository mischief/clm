// SPDX-License-Identifier: ISC
#ifndef CLM_HTTP_ASYNC_H
#define CLM_HTTP_ASYNC_H

#include <curl/curl.h>
#include <uv.h>

#include "clm/http.h"

/*
 * Opaque async-request handle. The full definition (curl + libuv state) lives
 * in http_async.c so no transport types leak into this header. Callers hold
 * the pointer and pass it to cancel/free only.
 */
struct clm_http_request;

/* HTTP completion callback typedefs now live in clm/http.h (included above). */

/*
 * Initiate an async HTTP POST request.
 *
 * api_key: if non-NULL and non-empty, adds an Authorization: Bearer header.
 * extra_headers: optional curl_slist of additional headers (request takes
 *   ownership and frees them on completion). Pass NULL for none.
 * client_suffix, when non-NULL, is appended to the User-Agent as a comment
 * "(tool: <suffix>)" so the server can attribute the request to a specific
 * tool/plugin. Pass NULL for the base User-Agent only.
 *
 * Returns 0 on success (request started), negative errno on failure.
 * On completion, either success_cb or error_cb will be called.
 */
int clm_http_async_post(uv_loop_t *loop, const char *url, const char *api_key,
                        const char *json_body, struct curl_slist *extra_headers,
                        clm_http_success_cb success_cb,
                        clm_http_error_cb error_cb, clm_http_data_cb data_cb,
                        const char *client_suffix, void *user,
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
