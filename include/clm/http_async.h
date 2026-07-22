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

/*
 * Opaque shared transport context: one CURLM multi handle plus the libuv
 * socket/timer glue driving it, bound to a single uv_loop_t for its whole
 * lifetime. Every clm_http_async_post() call against the same mux shares
 * curl's connection cache (TCP + TLS session reuse, not just DNS), instead of
 * each request paying for a fresh handshake -- and a fresh CA-bundle
 * load/parse -- from a private multi handle of its own.
 *
 * Scope one of these per logical "client" that makes repeat calls to the
 * same place over its lifetime (the desktop clm_host adapter uses one for
 * the whole process; each clm_mcp_client with an HTTP transport uses one for
 * that server). Never share a mux across independent clients -- there is no
 * reason one client's connection/TLS-session cache should serve another's
 * requests, and nothing here uses locks or threads, so "sharing" only ever
 * means "several requests multiplexed on the same uv-loop iteration", not
 * concurrent access from different threads.
 */
struct clm_http_mux;

/*
 * Create a mux bound to `loop`. The mux BORROWS the loop (same convention as
 * clm_host_uv_new): it registers handles but never runs or closes it.
 * Returns NULL on allocation failure.
 */
struct clm_http_mux *clm_http_mux_new(uv_loop_t *loop);

/*
 * Tear down a mux. Every request ever started against it must already have
 * completed, been cancelled, or otherwise been torn down before this is
 * called -- freeing a mux out from under a still-attached request would
 * leave that request holding a dangling CURLM*. This is an internal
 * lifetime invariant the caller is responsible for (not a validated input:
 * a mux is never handed a request from anyone else, so "still attached"
 * only happens if the caller freed it too early), so it is checked with
 * assert() rather than failing quietly -- continuing would corrupt memory
 * silently instead. Safe to call with NULL.
 */
void clm_http_mux_free(struct clm_http_mux *mux);

/*
 * Initiate an async HTTP POST request against a shared mux.
 *
 * api_key: if non-NULL and non-empty, adds an Authorization: Bearer header.
 * extra_headers: optional curl_slist of additional headers (request takes
 *   ownership and frees them on completion). Pass NULL for none.
 * client_suffix, when non-NULL, is appended to the User-Agent as a comment
 * "(tool: <suffix>)" so the server can attribute the request to a specific
 * tool/plugin. Pass NULL for the base User-Agent only.
 *
 * returns 0 when the request is accepted and negative errno when startup
 * fails. a failed start invokes no callback. an accepted request invokes
 * exactly one completion callback, which may run before this function returns.
 * out_req is cleared before startup and receives a cancellable handle only if
 * the request is still in flight when this function returns.
 */
int clm_http_async_post(struct clm_http_mux *mux, const char *url,
                        const char *api_key, const char *json_body,
                        struct curl_slist *extra_headers,
                        clm_http_success_cb success_cb,
                        clm_http_error_cb error_cb, clm_http_data_cb data_cb,
                        const char *client_suffix, void *user,
                        struct clm_http_request **out_req);

/*
 * abort an in-flight request. tears down its handles and delivers the outcome
 * to error_cb with -ECANCELED. safe to call once on the non-null handle returned
 * by clm_http_async_post, before the request has completed.
 */
void clm_http_async_cancel(struct clm_http_request *req);

/*
 * Free an async HTTP request and cleanup resources.
 * Called internally by the completion callbacks.
 */
void clm_http_request_free(struct clm_http_request *req);

#endif /* CLM_HTTP_ASYNC_H */
