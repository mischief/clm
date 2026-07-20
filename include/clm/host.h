// SPDX-License-Identifier: ISC
#ifndef CLM_HOST_H
#define CLM_HOST_H

/*
 * clm_host: the platform services the agent core needs, behind a tiny vtable.
 *
 * The core is loop- and transport-agnostic. It never includes <uv.h> or
 * <curl/curl.h>; instead the embedder supplies a struct clm_host with an HTTP
 * transport and (optionally) a one-shot timer, then hands it to clm_agent_new.
 *
 *   - Desktop:  a curl+libuv adapter (see the libclm-uv adapter).
 *   - Embedded: a blocking esp_http_client adapter, timer_set left NULL.
 *
 * This is deliberately NOT a general event-loop abstraction: the agent makes
 * one request at a time and never multiplexes file descriptors, so the surface
 * is just "make a request" and "run a timer", nothing more.
 */

#include <stddef.h>
#include <stdint.h>

#include "clm/http.h"

/* Opaque handles owned by the host implementation. */
struct clm_http_call; /* one in-flight HTTP request */
struct clm_timer;     /* one scheduled one-shot timer */

/*
 * A portable HTTP request description — no transport types leak in here.
 * headers, when non-NULL, is a NULL-terminated array of "Name: Value" strings
 * the request should send in addition to the defaults.
 */
struct clm_http_req {
	const char *url;
	const char *api_key;        /* Bearer auth; NULL or "" for none */
	const char *body;           /* request body; NULL => GET, else POST */
	const char *const *headers; /* NULL-terminated extra headers, or NULL */
	const char
	    *client_suffix; /* User-Agent "(tool: <suffix>)" tag, or NULL */
};

/* One-shot timer callback. */
typedef void (*clm_timer_cb)(void *arg);

struct clm_host {
	/*
	 * Start an HTTP request. Exactly one of success/error is invoked for it
	 * — later (async host) or before returning (blocking host). data may be
	 * NULL. On a 2xx streamed response, data receives body chunks; success
	 * still gets the full body. *out (if non-NULL) receives a handle usable
	 * with cancel. Returns 0 on accepted, negative errno on failure to
	 * start.
	 */
	int (*http_post)(void *ctx, const struct clm_http_req *req,
	                 clm_http_success_cb success, clm_http_error_cb error,
	                 clm_http_data_cb data, void *user,
	                 struct clm_http_call **out);

	/* Abort an in-flight request; delivers the error callback with
	 * -ECANCELED. Safe once, before completion; a no-op if already
	 * settling. */
	void (*http_cancel)(struct clm_http_call *call);

	/*
	 * Schedule a one-shot timer to fire cb(arg) after ms milliseconds.
	 * Optional: a host may leave timer_set NULL, in which case the core
	 * disables per-tool timeouts (a blocking transport enforces its own
	 * network timeout anyway). *out (if non-NULL) receives a handle usable
	 * with timer_cancel.
	 */
	int (*timer_set)(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
	                 struct clm_timer **out);

	/* Cancel and free a pending timer (the callback will not fire
	 * afterward). */
	void (*timer_cancel)(struct clm_timer *timer);

	void *ctx; /* opaque, passed to http_post/timer_set */

	/*
	 * The host's native event loop (e.g. the desktop adapter's
	 * uv_loop_t*), for tools that need to reach the underlying platform
	 * directly -- see clm_tool_invocation_loop(). NULL when the host has
	 * no such thing (e.g. a blocking embedded transport). Kept separate
	 * from ctx on purpose: ctx is whatever the adapter's own callbacks
	 * need (and may wrap more than the loop), while this is specifically
	 * the loop, for external consumers that must not depend on the
	 * adapter's private ctx layout.
	 */
	void *native_loop;
};

#endif /* CLM_HOST_H */
